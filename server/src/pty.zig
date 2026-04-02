const std = @import("std");
const posix = std.posix;
const builtin = @import("builtin");

pub const Pty = struct {
    master_fd: posix.fd_t,
    child_pid: posix.pid_t,

    pub fn spawn(argv: [*:null]const ?[*:0]const u8, env: [*:null]const ?[*:0]const u8) !Pty {
        var master_fd: posix.fd_t = undefined;
        var slave_fd: posix.fd_t = undefined;

        if (c_openpty(&master_fd, &slave_fd, null, null, null) != 0) {
            return error.OpenPtyFailed;
        }

        const pid = try posix.fork();

        if (pid == 0) {
            // Child
            posix.close(master_fd);
            _ = posix.setsid() catch {};
            _ = c_ioctl(slave_fd, TIOCSCTTY, 0);

            posix.dup2(slave_fd, 0) catch posix.exit(1);
            posix.dup2(slave_fd, 1) catch posix.exit(1);
            posix.dup2(slave_fd, 2) catch posix.exit(1);
            if (slave_fd > 2) posix.close(slave_fd);

            posix.execvpeZ(argv[0].?, argv, env) catch {};
            posix.exit(1);
        }

        // Parent
        posix.close(slave_fd);

        // Set non-blocking
        const F_GETFL: i32 = if (builtin.os.tag == .macos) 3 else 3;
        const F_SETFL: i32 = if (builtin.os.tag == .macos) 4 else 4;
        const O_NONBLOCK: usize = if (builtin.os.tag == .macos) 0x0004 else 0o4000;
        const flags = posix.fcntl(master_fd, F_GETFL, 0) catch 0;
        _ = posix.fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) catch {};

        return .{
            .master_fd = master_fd,
            .child_pid = pid,
        };
    }

    pub fn read(self: *const Pty, buf: []u8) !usize {
        return posix.read(self.master_fd, buf) catch |err| switch (err) {
            error.WouldBlock => return 0,
            else => return err,
        };
    }

    pub fn write(self: *const Pty, data: []const u8) !void {
        var written: usize = 0;
        while (written < data.len) {
            written += posix.write(self.master_fd, data[written..]) catch |err| switch (err) {
                error.WouldBlock => {
                    std.Thread.sleep(1_000_000);
                    continue;
                },
                else => return err,
            };
        }
    }

    pub fn resize(self: *const Pty, cols: u16, rows: u16) void {
        const ws = Winsize{
            .ws_row = rows,
            .ws_col = cols,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        _ = c_ioctl(self.master_fd, TIOCSWINSZ, @intFromPtr(&ws));
    }

    pub fn close(self: *Pty) void {
        posix.close(self.master_fd);
        _ = posix.waitpid(self.child_pid, 0);
    }

    pub fn isAlive(self: *const Pty) bool {
        const result = posix.waitpid(self.child_pid, posix.W.NOHANG);
        return result.pid == 0;
    }
};

const Winsize = extern struct {
    ws_row: u16,
    ws_col: u16,
    ws_xpixel: u16,
    ws_ypixel: u16,
};

const TIOCSWINSZ: c_ulong = if (builtin.os.tag == .macos) 0x80087467 else 0x5414;
const TIOCSCTTY: c_ulong = if (builtin.os.tag == .macos) 0x20007461 else 0x540E;

extern "c" fn openpty(
    amaster: *posix.fd_t,
    aslave: *posix.fd_t,
    name: ?[*:0]u8,
    termp: ?*anyopaque,
    winp: ?*anyopaque,
) c_int;

fn c_openpty(
    amaster: *posix.fd_t,
    aslave: *posix.fd_t,
    name: ?[*:0]u8,
    termp: ?*anyopaque,
    winp: ?*anyopaque,
) c_int {
    return openpty(amaster, aslave, name, termp, winp);
}

extern "c" fn ioctl(fd: c_int, request: c_ulong, ...) c_int;

fn c_ioctl(fd: posix.fd_t, request: c_ulong, arg: c_ulong) c_int {
    return ioctl(fd, request, arg);
}
