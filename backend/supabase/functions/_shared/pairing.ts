// Shared helpers for the 3.4 pairing functions.
import { createClient, SupabaseClient } from "npm:@supabase/supabase-js@2";

/** Pairing code lifetime. The device shows a countdown off expires_in. */
export const CODE_TTL_S = 600;
/** How often the device should poll. */
export const POLL_INTERVAL_S = 5;
/** Approved-but-uncollected rows linger this long past expiry, then get swept. */
export const SWEEP_GRACE_S = 900;

/** 32 characters: 2-9 and A-Z minus I and O. Every one exists in the
 *  handheld's 5x7 menu font, and none can be misread as another
 *  (no 0/O, no 1/I). 32 divides 256, so `byte & 31` has no modulo bias. */
export const ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
export const USER_CODE_LEN = 6; // 32^6 ~= 1.07e9

export function adminClient(): SupabaseClient {
  const url = Deno.env.get("SUPABASE_URL");
  const key = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!url || !key) throw new Error("missing SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY");
  return createClient(url, key, { auth: { persistSession: false, autoRefreshToken: false } });
}

export function newUserCode(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(USER_CODE_LEN));
  return Array.from(bytes, (b) => ALPHABET[b & 31]).join("");
}

/** 32 random bytes, base64url -- 43 chars, the device's secret. */
export function newDeviceCode(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export async function sha256Hex(s: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
}

/** Accept what a person types: lowercase, spaces, a dash in the middle. */
export function normalizeUserCode(raw: unknown): string | null {
  if (typeof raw !== "string") return null;
  const c = raw.toUpperCase().replace(/[\s-]/g, "");
  if (c.length !== USER_CODE_LEN) return null;
  for (const ch of c) if (!ALPHABET.includes(ch)) return null;
  return c;
}

/** Mint a session for a user server-side -- the 3.3d-validated path:
 *  admin.generateLink (sends no email) -> verifyOtp(token_hash). */
export async function mintSession(admin: SupabaseClient, userId: string) {
  const { data: u, error: e0 } = await admin.auth.admin.getUserById(userId);
  if (e0 || !u.user?.email) throw new Error(`getUserById: ${e0?.message ?? "no email"}`);

  const { data: link, error: e1 } = await admin.auth.admin.generateLink({ type: "magiclink", email: u.user.email });
  if (e1) throw new Error(`generateLink: ${e1.message}`);

  const { data: s, error: e2 } = await admin.auth.verifyOtp({ type: "magiclink", token_hash: link.properties.hashed_token });
  if (e2 || !s.session) throw new Error(`verifyOtp: ${e2?.message ?? "no session"}`);

  return {
    user_id: s.user!.id,
    email: s.user!.email,
    access_token: s.session.access_token,
    refresh_token: s.session.refresh_token,
    expires_in: s.session.expires_in,
  };
}

// The approve endpoint is called from the browser, so every response carries
// CORS headers and OPTIONS preflights are answered.
export const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, apikey, content-type, x-client-info",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

export function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...CORS, "Content-Type": "application/json" },
  });
}
