const std = @import("std");
const httpz = @import("httpz");
const tokens_mod = @import("tokens");

const Role = tokens_mod.Role;

pub const Client = struct {
    role: Role,
    conn: *httpz.websocket.Conn,
};

pub const Session = struct {
    clients: std.ArrayList(Client),
    allocator: std.mem.Allocator,
    status: Status,
    mu: std.Thread.Mutex,

    pub const Status = enum {
        booting,
        installing_agent,
        starting_agent,
        ready,
    };

    pub fn init(allocator: std.mem.Allocator) Session {
        return .{
            .clients = .empty,
            .allocator = allocator,
            .status = .booting,
            .mu = .{},
        };
    }

    pub fn deinit(self: *Session) void {
        self.clients.deinit(self.allocator);
    }

    pub fn addClient(self: *Session, conn: *httpz.websocket.Conn, role: Role) !void {
        self.mu.lock();
        defer self.mu.unlock();
        try self.clients.append(self.allocator, .{ .role = role, .conn = conn });
    }

    pub fn removeClient(self: *Session, conn: *httpz.websocket.Conn) void {
        self.mu.lock();
        defer self.mu.unlock();
        for (self.clients.items, 0..) |client, i| {
            if (client.conn == conn) {
                _ = self.clients.swapRemove(i);
                return;
            }
        }
    }

    pub fn setStatus(self: *Session, status: Status) void {
        self.mu.lock();
        defer self.mu.unlock();
        self.status = status;
    }

    pub fn getStatus(self: *Session) Status {
        self.mu.lock();
        defer self.mu.unlock();
        return self.status;
    }

    pub fn clientCount(self: *Session) usize {
        self.mu.lock();
        defer self.mu.unlock();
        return self.clients.items.len;
    }

    pub fn broadcast(self: *Session, message: []const u8) void {
        self.mu.lock();
        defer self.mu.unlock();
        for (self.clients.items) |client| {
            client.conn.write(message) catch {};
        }
    }
};
