import { useState, type FormEvent } from 'react'
import { supabase } from './supabase'

export default function Login() {
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  async function onSubmit(e: FormEvent) {
    e.preventDefault()
    setBusy(true)
    setError(null)
    const { error } = await supabase.auth.signInWithPassword({ email, password })
    // On success, App's onAuthStateChange swaps this page out.
    if (error) setError(error.message)
    setBusy(false)
  }

  return (
    <form className="card" onSubmit={onSubmit}>
      <h1>Sign in</h1>
      <p className="muted">Sign in to link your handheld and see your saves.</p>

      <label>
        Email
        <input type="email" autoComplete="email" required
               value={email} onChange={(e) => setEmail(e.target.value)} />
      </label>

      <label>
        Password
        <input type="password" autoComplete="current-password" required
               value={password} onChange={(e) => setPassword(e.target.value)} />
      </label>

      {error && <p className="error" role="alert">{error}</p>}

      <button type="submit" disabled={busy}>{busy ? 'Signing in…' : 'Sign in'}</button>
    </form>
  )
}
