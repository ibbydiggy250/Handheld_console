// 3.4 device-issue -- called by an unpaired handheld (no user session).
// Returns a secret device_code (kept by the device) and a short user_code
// (shown on screen). Only the hash of device_code is stored.
import {
  adminClient, CODE_TTL_S, CORS, json, newDeviceCode, newUserCode,
  POLL_INTERVAL_S, sha256Hex, SWEEP_GRACE_S,
} from "../_shared/pairing.ts";

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response(null, { headers: CORS });
  if (req.method !== "POST") return json({ error: "method_not_allowed" }, 405);

  const admin = adminClient();

  // Sweep: expired rows (plus a grace window for approved-but-uncollected ones).
  // Keeps the table small and frees user_codes for reuse.
  const cutoff = new Date(Date.now() - SWEEP_GRACE_S * 1000).toISOString();
  await admin.from("device_codes").delete().lt("expires_at", cutoff);

  const deviceCode = newDeviceCode();
  const deviceHash = await sha256Hex(deviceCode);
  const expiresAt = new Date(Date.now() + CODE_TTL_S * 1000).toISOString();

  // user_code is unique; on the rare collision, draw again.
  for (let attempt = 0; attempt < 5; attempt++) {
    const userCode = newUserCode();
    const { error } = await admin.from("device_codes").insert({
      device_code_hash: deviceHash,
      user_code: userCode,
      expires_at: expiresAt,
    });
    if (!error) {
      return json({
        device_code: deviceCode,
        user_code: userCode,
        expires_in: CODE_TTL_S,
        interval: POLL_INTERVAL_S,
      });
    }
    if (error.code !== "23505") return json({ error: "insert_failed", detail: error.message }, 500);
  }
  return json({ error: "code_space_exhausted" }, 503);
});
