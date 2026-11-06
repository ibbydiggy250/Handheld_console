# Handheld — web app

React + TypeScript (Vite) frontend for the handheld's cloud saves.
Phase 3.4: sign in, then link a handheld by typing the code on its screen.

```bash
cp .env.example .env.local   # fill in project URL + publishable key
npm install
npm run dev                  # http://localhost:5173
```

Talks to Supabase with the **publishable** key only. Everything it can do is
bounded by RLS (`backend/schema.sql`) and the Edge Functions
(`backend/supabase/functions/`).
