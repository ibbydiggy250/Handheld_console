-- Phase 3.4 -- device pairing (device-code flow, cf. RFC 8628).
-- Paste into Supabase SQL Editor and Run, once.
--
-- Two codes per pairing:
--   user_code   6 chars, shown on the handheld, typed by the user. Can only
--               APPROVE. Visible to anyone looking at the screen.
--   device_code 32 random bytes, known only to the device. Only it can
--               COLLECT the session. Stored here as a sha256 hash, so a
--               leaked table can't be used to collect anything.

create table public.device_codes (
  id               bigint generated always as identity primary key,
  device_code_hash text not null unique,
  user_code        text not null unique,
  user_id          uuid references auth.users(id) on delete cascade,  -- null until approved
  created_at       timestamptz not null default now(),
  expires_at       timestamptz not null,
  approved_at      timestamptz
);

-- RLS on with NO policies: no client (device, browser, anon or signed-in) can
-- read or write this table directly. Only the Edge Functions, which use the
-- service-role key, touch it.
alter table public.device_codes enable row level security;
