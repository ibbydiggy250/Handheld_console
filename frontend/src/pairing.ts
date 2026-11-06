// Must match backend/supabase/functions/_shared/pairing.ts and the handheld's
// font: 2-9 and A-Z without I and O.
export const ALPHABET = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ'
export const CODE_LEN = 6

/** Keep only characters that can appear in a code, uppercased, max 6. */
export function cleanCode(raw: string): string {
  let out = ''
  for (const ch of raw.toUpperCase()) {
    if (ALPHABET.includes(ch)) out += ch
    if (out.length === CODE_LEN) break
  }
  return out
}

/** "K7QXM4" -> "K7Q-XM4", the way the handheld shows it. */
export function formatCode(code: string): string {
  return code.length > 3 ? `${code.slice(0, 3)}-${code.slice(3)}` : code
}

/** Map device-approve's error codes to something a person can act on. */
export function approveErrorMessage(code: string | undefined): string {
  switch (code) {
    case 'invalid_or_expired':
      return "That code doesn't match a device that's waiting, or it expired. Check the handheld's screen — it shows a fresh code if the old one ran out."
    case 'invalid_code':
      return 'Codes are 6 characters: digits 2–9 and letters, no I or O.'
    case 'not_signed_in':
      return 'Your sign-in expired. Sign out and back in, then try again.'
    default:
      return 'Something went wrong linking the device. Try again in a moment.'
  }
}
