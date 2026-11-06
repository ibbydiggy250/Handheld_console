// Phase 3.3d spike -- can an Edge Function mint a session server-side?
// Path under test: admin.generateLink (no email sent) -> verifyOtp(token_hash).
// If this works, 3.4 can hand a paired device its own session without the
// device ever seeing a password.
//
// SAFETY: mints only for the CALLER (identity from their JWT). The request
// body is ignored, so even if this stays deployed it can't be used to become
// someone else. Delete it after the spike anyway.

import { createClient } from "npm:@supabase/supabase-js@2";

Deno.serve(async (req) => {
  const url = Deno.env.get("SUPABASE_URL");
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!url || !serviceKey) {
    return Response.json({ step: "env", error: "missing SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY" }, { status: 500 });
  }

  const admin = createClient(url, serviceKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });

  // Who is calling? Taken from their JWT, never from the body.
  const jwt = (req.headers.get("Authorization") ?? "").replace(/^Bearer\s+/i, "");
  const { data: caller, error: e0 } = await admin.auth.getUser(jwt);
  if (e0 || !caller.user?.email) {
    return Response.json({ step: "getUser", error: e0?.message ?? "no user" }, { status: 401 });
  }

  // 1. Server-side magic link. Returns the hashed token; sends no email.
  const { data: link, error: e1 } = await admin.auth.admin.generateLink({
    type: "magiclink",
    email: caller.user.email,
  });
  if (e1) return Response.json({ step: "generateLink", error: e1.message }, { status: 500 });

  // 2. Redeem it immediately for a session.
  const { data: s, error: e2 } = await admin.auth.verifyOtp({
    type: "magiclink",
    token_hash: link.properties.hashed_token,
  });
  if (e2 || !s.session) {
    return Response.json({ step: "verifyOtp", error: e2?.message ?? "no session" }, { status: 500 });
  }

  return Response.json({
    user_id: s.user!.id,
    access_token: s.session.access_token,
    refresh_token: s.session.refresh_token,
    expires_in: s.session.expires_in,
  });
});
