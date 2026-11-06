// 3.4 device-approve -- called from the website by a signed-in user.
// Body: { "code": "K7QXM4" } (case, spaces and a dash are tolerated).
// Links that pending code to the caller's account.
import { adminClient, CORS, json, normalizeUserCode } from "../_shared/pairing.ts";

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { headers: CORS });
  if (req.method !== "POST") return json({ error: "method_not_allowed" }, 405);

  const admin = adminClient();

  // Who is approving? From their JWT -- the gateway check is off (config.toml),
  // so this is the only authentication on this endpoint.
  const jwt = (req.headers.get("Authorization") ?? "").replace(/^Bearer\s+/i, "");
  const { data: caller, error: authErr } = await admin.auth.getUser(jwt);
  if (authErr || !caller.user) return json({ error: "not_signed_in" }, 401);

  let body: { code?: unknown };
  try { body = await req.json(); } catch { return json({ error: "bad_json" }, 400); }

  const code = normalizeUserCode(body.code);
  if (!code) return json({ error: "invalid_code" }, 400);

  // One conditional UPDATE: the code must exist, be unclaimed, and be
  // unexpired. Atomic, so two people racing the same code can't both win.
  const { data, error } = await admin
    .from("device_codes")
    .update({ user_id: caller.user.id, approved_at: new Date().toISOString() })
    .eq("user_code", code)
    .is("user_id", null)
    .gt("expires_at", new Date().toISOString())
    .select("id");

  if (error) return json({ error: "update_failed", detail: error.message }, 500);

  // Same answer for wrong / expired / already-claimed: don't tell a guesser
  // which codes exist.
  if (!data || data.length === 0) return json({ error: "invalid_or_expired" }, 404);

  return json({ ok: true });
});
