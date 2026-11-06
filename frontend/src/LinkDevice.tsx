import { useState, type FormEvent } from 'react'
import { FunctionsHttpError } from '@supabase/supabase-js'
import { supabase } from './supabase'
import { approveErrorMessage, cleanCode, CODE_LEN, formatCode } from './pairing'

type Status =
  | { kind: 'idle' }
  | { kind: 'busy' }
  | { kind: 'linked'; code: string }
  | { kind: 'error'; message: string }

export default function LinkDevice() {
  const [code, setCode] = useState('')          // cleaned, no dash
  const [status, setStatus] = useState<Status>({ kind: 'idle' })

  async function onSubmit(e: FormEvent) {
    e.preventDefault()
    if (code.length !== CODE_LEN) return
    setStatus({ kind: 'busy' })

    // invoke() sends the apikey and the signed-in user's JWT for us.
    const { error } = await supabase.functions.invoke('device-approve', { body: { code } })

    if (!error) {
      setStatus({ kind: 'linked', code })
      setCode('')
      return
    }

    let errCode: string | undefined
    if (error instanceof FunctionsHttpError) {
      try { errCode = (await error.context.json()).error } catch { /* non-JSON body */ }
    }
    setStatus({ kind: 'error', message: approveErrorMessage(errCode) })
  }

  return (
    <form className="card" onSubmit={onSubmit}>
      <h1>Link device</h1>
      <p className="muted">
        On the handheld, choose <b>Link device</b>. Type the 6-character code it shows.
      </p>

      <input
        className="code"
        aria-label="Pairing code"
        placeholder="ABC-234"
        autoComplete="off"
        autoCapitalize="characters"
        spellCheck={false}
        inputMode="text"
        value={formatCode(code)}
        onChange={(e) => {
          setCode(cleanCode(e.target.value))
          if (status.kind !== 'busy') setStatus({ kind: 'idle' })
        }}
      />

      {status.kind === 'linked' && (
        <p className="ok" role="status">
          Linked ✓ — the handheld picks this up within a few seconds.
        </p>
      )}
      {status.kind === 'error' && <p className="error" role="alert">{status.message}</p>}

      <button type="submit" disabled={code.length !== CODE_LEN || status.kind === 'busy'}>
        {status.kind === 'busy' ? 'Linking…' : 'Link'}
      </button>
    </form>
  )
}
