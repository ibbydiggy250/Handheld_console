-- Phase 3.3 -- save-state backend. Paste into Supabase SQL Editor and Run.
-- Blobs are opaque to the backend; the device and web app own the format.

-- ------------------------------------------------------------ table

create table public.save_states (
  id             bigint generated always as identity primary key,
  user_id        uuid not null default auth.uid()
                 references auth.users(id) on delete cascade,
  game_id        text not null,          -- sha256 of ROM bytes, hex
  rom_name       text,                   -- advisory, from the blob header
  path           text not null unique,   -- {user_id}/{game_id}/{ts}.c8st
  size_bytes     int  not null,
  format         text not null default 'c8st',
  format_version smallint not null default 1,
  created_at     timestamptz not null default now()
);

-- "latest save for this user + game" (3.7)
create index on public.save_states (user_id, game_id, created_at desc);

alter table public.save_states enable row level security;

create policy "read own" on public.save_states
  for select to authenticated
  using ((select auth.uid()) = user_id);

-- path must live in the caller's own folder
create policy "insert own" on public.save_states
  for insert to authenticated
  with check (
    (select auth.uid()) = user_id
    and path like (select auth.uid())::text || '/%'
  );

create policy "delete own" on public.save_states
  for delete to authenticated
  using ((select auth.uid()) = user_id);

-- No update policy: full history is kept, a save is immutable once written.

-- ------------------------------------------------------------ storage

insert into storage.buckets (id, name, public, file_size_limit)
values ('saves', 'saves', false, 1048576);   -- 1 MB; GB states will be larger

create policy "read own folder" on storage.objects
  for select to authenticated
  using (bucket_id = 'saves'
         and (storage.foldername(name))[1] = (select auth.uid())::text);

create policy "write own folder" on storage.objects
  for insert to authenticated
  with check (bucket_id = 'saves'
              and (storage.foldername(name))[1] = (select auth.uid())::text);
