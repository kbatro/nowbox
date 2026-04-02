const std = @import("std");

const Sha256 = std.crypto.hash.sha2.Sha256;

pub const Role = enum {
    owner,
    collaborator,
};

pub const TokenEntry = struct {
    hash: [Sha256.digest_length]u8,
    role: Role,
};

const base58_alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

pub const Tokens = struct {
    entries: std.ArrayList(TokenEntry),
    allocator: std.mem.Allocator,
    root_token_plain: [44]u8,

    pub fn init(allocator: std.mem.Allocator) Tokens {
        var self = Tokens{
            .entries = .empty,
            .allocator = allocator,
            .root_token_plain = undefined,
        };

        self.root_token_plain = generateToken();
        const hash = hashToken(&self.root_token_plain);
        self.entries.append(allocator, .{ .hash = hash, .role = .owner }) catch unreachable;

        return self;
    }

    pub fn deinit(self: *Tokens) void {
        self.entries.deinit(self.allocator);
    }

    pub fn validate(self: *const Tokens, token: []const u8) ?Role {
        if (token.len == 0 or token.len > 64) return null;

        var hash: [Sha256.digest_length]u8 = undefined;
        Sha256.hash(token, &hash, .{});

        for (self.entries.items) |entry| {
            if (std.mem.eql(u8, &entry.hash, &hash)) {
                return entry.role;
            }
        }
        return null;
    }

    pub fn createShareToken(self: *Tokens) ![44]u8 {
        const token = generateToken();
        const hash = hashToken(&token);
        try self.entries.append(self.allocator, .{ .hash = hash, .role = .collaborator });
        return token;
    }

    pub fn revokeAll(self: *Tokens) usize {
        var removed: usize = 0;
        var i: usize = 0;
        while (i < self.entries.items.len) {
            if (i == 0) {
                i += 1;
                continue;
            }
            if (self.entries.items[i].role == .collaborator) {
                _ = self.entries.swapRemove(i);
                removed += 1;
            } else {
                i += 1;
            }
        }
        return removed;
    }

    pub fn rootToken(self: *const Tokens) []const u8 {
        return &self.root_token_plain;
    }
};

fn generateToken() [44]u8 {
    var raw: [32]u8 = undefined;
    std.crypto.random.bytes(&raw);
    return encodeBase58(&raw);
}

fn hashToken(token: []const u8) [Sha256.digest_length]u8 {
    var hash: [Sha256.digest_length]u8 = undefined;
    Sha256.hash(token, &hash, .{});
    return hash;
}

fn encodeBase58(input: []const u8) [44]u8 {
    var result: [44]u8 = [_]u8{'1'} ** 44;
    var num: [32]u8 = undefined;
    @memcpy(&num, input);

    var idx: usize = 43;
    var is_zero = false;

    while (!is_zero) {
        var remainder: u16 = 0;
        is_zero = true;

        for (&num) |*byte| {
            const acc: u16 = remainder * 256 + byte.*;
            byte.* = @intCast(acc / 58);
            remainder = acc % 58;
            if (byte.* != 0) is_zero = false;
        }

        result[idx] = base58_alphabet[@intCast(remainder)];
        if (idx == 0) break;
        idx -= 1;
    }

    return result;
}
