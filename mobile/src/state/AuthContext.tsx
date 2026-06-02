// AuthContext: stores the operator's backend URL + API token in
// expo-secure-store (encrypted at rest on iOS Keychain / Android
// Keystore) and exposes them via a useAuth() hook. Every screen
// that needs to call the API reads from here; saving / clearing
// from the Login screen propagates to the rest of the tree.

import * as SecureStore from 'expo-secure-store';
import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
} from 'react';

const KEY_BASE_URL = 'hft.baseUrl';
const KEY_TOKEN = 'hft.token';

export interface AuthState {
  baseUrl: string | null;
  token: string | null;
  ready: boolean; // true once the initial SecureStore read finishes
  save: (baseUrl: string, token: string) => Promise<void>;
  clear: () => Promise<void>;
}

const AuthCtx = createContext<AuthState | null>(null);

export const AuthProvider: React.FC<{ children: React.ReactNode }> = ({
  children,
}) => {
  const [baseUrl, setBaseUrl] = useState<string | null>(null);
  const [token, setToken] = useState<string | null>(null);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    (async () => {
      const [u, t] = await Promise.all([
        SecureStore.getItemAsync(KEY_BASE_URL),
        SecureStore.getItemAsync(KEY_TOKEN),
      ]);
      setBaseUrl(u);
      setToken(t);
      setReady(true);
    })();
  }, []);

  const save = useCallback(async (u: string, t: string) => {
    // Normalise: trim, strip trailing slash so the API client's
    // path concatenation doesn't double-slash.
    const cleanUrl = u.trim().replace(/\/$/, '');
    const cleanToken = t.trim();
    await Promise.all([
      SecureStore.setItemAsync(KEY_BASE_URL, cleanUrl),
      SecureStore.setItemAsync(KEY_TOKEN, cleanToken),
    ]);
    setBaseUrl(cleanUrl);
    setToken(cleanToken);
  }, []);

  const clear = useCallback(async () => {
    await Promise.all([
      SecureStore.deleteItemAsync(KEY_BASE_URL),
      SecureStore.deleteItemAsync(KEY_TOKEN),
    ]);
    setBaseUrl(null);
    setToken(null);
  }, []);

  const value = useMemo<AuthState>(
    () => ({ baseUrl, token, ready, save, clear }),
    [baseUrl, token, ready, save, clear],
  );

  return <AuthCtx.Provider value={value}>{children}</AuthCtx.Provider>;
};

export function useAuth(): AuthState {
  const ctx = useContext(AuthCtx);
  if (!ctx) throw new Error('useAuth used outside AuthProvider');
  return ctx;
}
