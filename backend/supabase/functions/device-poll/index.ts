// 3.4 device-poll -- called by the handheld every `interval` seconds.
// Body: { "device_code": "<secret from device-issue>" }
// Returns { status: "pending" | "expired" | "invalid" } or, exactly once,
// { status: "approved", user_id, access_token, refresh_token, expires_in }.
import { adminClient, CORS, json, mintSession, sha256Hex } from "../_shared/pairing.ts";

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { headers: CORS });
  if (req.method !== "POST") return json({ error: "method_not_allowed" }, 405);

  let body: { device_code?: unknown };
  try { body = await req.json(); } catch { return json({ error: "bad_json" }, 400); }
  if (typeof body.device_code !== "string" || body.device_code.length < 32) {
    return json({ status: "invalid" }, 400);
  }

  const admin = adminClient();
  const hash = await sha256Hex(body.device_code);

  const { data: row, error } = await admin
    .from("device_codes")
    .select("id, user_id, expires_at")
    .eq("device_code_hash", hash)
    .maybeSingle();

  if (error) return json({ error: "lookup_failed", detail: error.message }, 500);
  if (!row) return json({ status: "invalid" }, 404);   // never issued, or already collected

  if (!row.user_id) {
    // Unapproved: expiry applies. Once approved, the device may still collect
    // until the sweep (expiry + grace) removes the row.
    if (new Date(row.expires_at) <= new Date()) return json({ status: "expired" }, 410);
    return json({ status: "pending" });
  }

  // Approved. Claim it by DELETING the row -- only one poll can delete it, so
  // only one poll gets a session, even if the device double-sends.
  const { data: claimed, error: claimErr } = await admin
    .from("device_codes")
    .delete()
    .eq("id", row.id)
    .select("user_id");

  if (claimErr) return json({ error: "claim_failed", detail: claimErr.message }, 500);
  if (!claimed || claimed.length === 0) return json({ status: "invalid" }, 404);

  try {
    const session = await mintSession(admin, claimed[0].user_id);
    return json({ status: "approved", ...session });
  } catch (e) {
    // Row is already gone; the user re-pairs. Rare, and safer than minting twice.
    return json({ error: "mint_failed", detail: String(e) }, 500);
  }
});
