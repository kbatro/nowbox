const std = @import("std");
const httpz = @import("httpz");
const tokens_mod = @import("tokens");
const session_mod = @import("session");
const pty_mod = @import("pty");

const Tokens = tokens_mod.Tokens;
const Session = session_mod.Session;
const Pty = pty_mod.Pty;

const index_html = @embedFile("static/index.html");
const style_css = @embedFile("static/style.css");
const app_js = @embedFile("static/app.js");

const App = struct {
    tokens: Tokens,
    session: Session,
    pty: ?Pty,
    allocator: std.mem.Allocator,

    pub const WebsocketHandler = WsClient;

    pub fn init(allocator: std.mem.Allocator) App {
        return .{
            .tokens = Tokens.init(allocator),
            .session = Session.init(allocator),
            .pty = null,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *App) void {
        if (self.pty) |*p| p.close();
        self.session.deinit();
        self.tokens.deinit();
    }
};

const WsClient = struct {
    app: *App,
    role: ?tokens_mod.Role,
    conn: *httpz.websocket.Conn,
    authenticated: bool,

    const Context = struct {
        app: *App,
    };

    pub fn init(conn: *httpz.websocket.Conn, ctx: *const Context) !WsClient {
        return .{
            .app = ctx.app,
            .role = null,
            .conn = conn,
            .authenticated = false,
        };
    }

    pub fn afterInit(self: *WsClient) !void {
        return self.conn.write(
            \\{"type":"auth_required"}
        );
    }

    pub fn clientMessage(self: *WsClient, data: []const u8) !void {
        const parsed = std.json.parseFromSlice(std.json.Value, self.app.allocator, data, .{}) catch {
            return self.conn.write(
                \\{"type":"error","message":"invalid json"}
            );
        };
        defer parsed.deinit();

        const obj = parsed.value.object;
        const msg_type = (obj.get("type") orelse return).string;

        if (!self.authenticated) {
            if (!std.mem.eql(u8, msg_type, "auth")) {
                return self.conn.write(
                    \\{"type":"error","message":"authenticate first"}
                );
            }
            return self.handleAuth(obj);
        }

        if (std.mem.eql(u8, msg_type, "terminal")) return self.handleTerminal(obj);
        if (std.mem.eql(u8, msg_type, "chat")) return self.handleChat(obj);
        if (std.mem.eql(u8, msg_type, "resize")) return self.handleResize(obj);
        if (std.mem.eql(u8, msg_type, "create_share_token")) return self.handleCreateShareToken();
        if (std.mem.eql(u8, msg_type, "status")) return self.sendStatus();
    }

    fn handleAuth(self: *WsClient, obj: std.json.ObjectMap) !void {
        const token = (obj.get("token") orelse return).string;
        const role = self.app.tokens.validate(token);

        if (role) |r| {
            self.authenticated = true;
            self.role = r;
            try self.app.session.addClient(self.conn, r);

            const role_str = if (r == .owner) "owner" else "collaborator";
            var buf: [128]u8 = undefined;
            const msg = std.fmt.bufPrint(&buf,
                \\{{"type":"auth_ok","role":"{s}"}}
            , .{role_str}) catch return;
            try self.conn.write(msg);
            try self.sendStatus();
            self.broadcastUsers();
            return;
        }

        try self.conn.write(
            \\{"type":"auth_fail"}
        );
    }

    fn handleTerminal(self: *WsClient, obj: std.json.ObjectMap) !void {
        const p = self.app.pty orelse return;
        const data_b64 = (obj.get("data") orelse return).string;

        var decode_buf: [4096]u8 = undefined;
        const decoded_len = std.base64.standard.Decoder.calcSizeForSlice(data_b64) catch return;
        if (decoded_len > decode_buf.len) return;
        std.base64.standard.Decoder.decode(&decode_buf, data_b64) catch return;
        p.write(decode_buf[0..decoded_len]) catch {};
    }

    fn handleChat(self: *WsClient, obj: std.json.ObjectMap) !void {
        const p = self.app.pty orelse return;
        const message = (obj.get("message") orelse return).string;

        var buf: [4096]u8 = undefined;
        if (message.len + 1 > buf.len) return;
        @memcpy(buf[0..message.len], message);
        buf[message.len] = '\n';
        p.write(buf[0 .. message.len + 1]) catch {};
    }

    fn handleResize(self: *WsClient, obj: std.json.ObjectMap) !void {
        const p = self.app.pty orelse return;
        const cols: u16 = @intCast((obj.get("cols") orelse return).integer);
        const rows: u16 = @intCast((obj.get("rows") orelse return).integer);
        p.resize(cols, rows);
    }

    fn handleCreateShareToken(self: *WsClient) !void {
        if (self.role != .owner) {
            return self.conn.write(
                \\{"type":"error","message":"owner only"}
            );
        }
        const token = self.app.tokens.createShareToken() catch return;
        var buf: [128]u8 = undefined;
        const msg = std.fmt.bufPrint(&buf,
            \\{{"type":"share_token","token":"{s}"}}
        , .{token}) catch return;
        try self.conn.write(msg);
    }

    fn sendStatus(self: *WsClient) !void {
        const status_str = switch (self.app.session.getStatus()) {
            .booting => "booting",
            .installing_agent => "installing_agent",
            .starting_agent => "starting_agent",
            .ready => "ready",
        };
        var buf: [128]u8 = undefined;
        const msg = std.fmt.bufPrint(&buf,
            \\{{"type":"status","stage":"{s}"}}
        , .{status_str}) catch return;
        try self.conn.write(msg);
    }

    fn broadcastUsers(self: *WsClient) void {
        const count = self.app.session.clientCount();
        var buf: [64]u8 = undefined;
        const msg = std.fmt.bufPrint(&buf,
            \\{{"type":"users","count":{d}}}
        , .{count}) catch return;
        self.app.session.broadcast(msg);
    }

    pub fn close(self: *WsClient) void {
        self.app.session.removeClient(self.conn);
        self.broadcastUsers();
    }
};

fn serveIndex(_: *App, _: *httpz.Request, res: *httpz.Response) !void {
    res.content_type = .HTML;
    res.body = index_html;
}

fn serveStyle(_: *App, _: *httpz.Request, res: *httpz.Response) !void {
    res.content_type = .CSS;
    res.body = style_css;
}

fn serveJs(_: *App, _: *httpz.Request, res: *httpz.Response) !void {
    res.content_type = .JS;
    res.body = app_js;
}

fn handleWsUpgrade(app: *App, req: *httpz.Request, res: *httpz.Response) !void {
    const ctx = WsClient.Context{ .app = app };
    if (try httpz.upgradeWebsocket(WsClient, req, res, &ctx) == false) {
        res.status = 400;
        res.body = "websocket upgrade failed";
    }
}

fn ptyReader(app: *App) void {
    var buf: [8192]u8 = undefined;
    var encode_buf: [12288]u8 = undefined;

    while (true) {
        const p = app.pty orelse {
            std.Thread.sleep(100_000_000);
            continue;
        };

        const n = p.read(&buf) catch break;
        if (n == 0) {
            std.Thread.sleep(10_000_000);
            continue;
        }

        const encoded = std.base64.standard.Encoder.encode(&encode_buf, buf[0..n]);

        var msg_buf: [16384]u8 = undefined;
        const msg = std.fmt.bufPrint(&msg_buf,
            \\{{"type":"terminal","data":"{s}"}}
        , .{encoded}) catch continue;

        app.session.broadcast(msg);
    }
}

fn print(comptime fmt: []const u8, args: anytype) void {
    const f = std.fs.File{ .handle = std.posix.STDOUT_FILENO };
    f.writeAll(std.fmt.comptimePrint(fmt, args)) catch {};
}

pub fn main() !void {
    var gpa: std.heap.GeneralPurposeAllocator(.{}) = .init;
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var app = App.init(allocator);
    defer app.deinit();

    std.debug.print("\n  nowbox-server starting on :8080\n", .{});
    std.debug.print("  root token: {s}\n\n", .{app.tokens.rootToken()});

    // Spawn agent (default to /bin/sh for local dev)
    const agent_cmd = std.process.getEnvVarOwned(allocator, "NOWBOX_AGENT") catch |err| switch (err) {
        error.EnvironmentVariableNotFound => try allocator.dupe(u8, "/bin/sh"),
        else => return err,
    };
    defer allocator.free(agent_cmd);

    const agent_cmd_z = try allocator.dupeZ(u8, agent_cmd);
    defer allocator.free(agent_cmd_z);

    const argv: [2:null]?[*:0]const u8 = .{ agent_cmd_z.ptr, null };

    app.session.setStatus(.starting_agent);
    app.pty = Pty.spawn(&argv, std.c.environ) catch |err| {
        std.debug.print("  failed to spawn agent: {}\n", .{err});
        return err;
    };
    app.session.setStatus(.ready);
    std.debug.print("  agent started (pid: {d})\n", .{app.pty.?.child_pid});

    // PTY reader thread
    const pty_thread = try std.Thread.spawn(.{}, ptyReader, .{&app});
    pty_thread.detach();

    // HTTP server
    var server = try httpz.Server(*App).init(allocator, .{
        .address = .all(8080),
    }, &app);
    defer server.deinit();
    defer server.stop();

    var router = try server.router(.{});
    router.get("/", serveIndex, .{});
    router.get("/style.css", serveStyle, .{});
    router.get("/app.js", serveJs, .{});
    router.get("/ws", handleWsUpgrade, .{});

    std.debug.print("  listening...\n\n", .{});
    try server.listen();
}
