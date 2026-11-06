import { useEffect, useState } from 'react'
import type { Session } from '@supabase/supabase-js'
import { supabase } from './supabase'
import Login from './Login'
import LinkDevice from './LinkDevice'

export default function App() {
  // undefined = still restoring a saved session; null = signed out.
  const [session, setSession] = useState<Session | null | undefined>(undefined)

  useEffect(() => {
    supabase.auth.getSession().then(({ data }) => setSession(data.session))
    const { data: sub } = supabase.auth.onAuthStateChange((_event, s) => setSession(s))
    return () => sub.subscription.unsubscribe()
  }, [])

  return (
    <div className="page">
      <header>
        <span className="brand">HANDHELD</span>
        {session && (
          <span className="who">
            {session.user.email}
            <button className="link" onClick={() => supabase.auth.signOut()}>Sign out</button>
          </span>
        )}
      </header>

      <main>
        {session === undefined ? null : session ? <LinkDevice /> : <Login />}
      </main>
    </div>
  )
}
