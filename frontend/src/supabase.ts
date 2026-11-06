import { createClient } from '@supabase/supabase-js'

const url = import.meta.env.VITE_SUPABASE_URL as string | undefined
const key = import.meta.env.VITE_SUPABASE_PUBLISHABLE_KEY as string | undefined

if (!url || !key) {
  throw new Error('Missing VITE_SUPABASE_URL / VITE_SUPABASE_PUBLISHABLE_KEY -- copy .env.example to .env.local')
}

// Publishable key only: everything this client can do is bounded by RLS and
// by what the Edge Functions choose to allow.
export const supabase = createClient(url, key)
