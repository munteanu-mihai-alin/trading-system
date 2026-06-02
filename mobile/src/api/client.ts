// Thin fetch wrapper that injects the X-HFT-Token header on every
// call. All screens go through this so the auth handling lives in
// one place; if the token rolls (operator regenerates /etc/hft/api.env)
// we only touch this file.
//
// 401 is the canonical "your token is wrong" - the wrapper throws
// AuthError which screens catch and route back to Login.

import type {
  BacktestQueue,
  DatabentoCreditsResponse,
  HealthResponse,
  LaunchBacktestBody,
  LaunchBacktestResponse,
  LiveStatusResponse,
  RunDetail,
  RunSummary,
  SignalResponse,
} from './types';

export class AuthError extends Error {
  constructor() {
    super('unauthorized: bad token or backend URL');
    this.name = 'AuthError';
  }
}

export class ApiError extends Error {
  status: number;
  body: string;
  constructor(status: number, body: string) {
    super(`api error ${status}: ${body.slice(0, 200)}`);
    this.status = status;
    this.body = body;
    this.name = 'ApiError';
  }
}

interface Args {
  baseUrl: string;
  token: string;
}

async function _request<T>(
  { baseUrl, token }: Args,
  path: string,
  init?: RequestInit,
): Promise<T> {
  const res = await fetch(`${baseUrl}${path}`, {
    ...init,
    headers: {
      'Content-Type': 'application/json',
      'X-HFT-Token': token,
      ...(init?.headers ?? {}),
    },
  });
  if (res.status === 401) throw new AuthError();
  const text = await res.text();
  if (!res.ok) throw new ApiError(res.status, text);
  // FastAPI always returns JSON; empty body is rare but defensible.
  return text ? (JSON.parse(text) as T) : ({} as T);
}

export const api = {
  health: (a: Args) => _request<HealthResponse>(a, '/health'),
  liveStatus: (a: Args) => _request<LiveStatusResponse>(a, '/live/status'),
  runs: (a: Args) =>
    _request<{ runs: RunSummary[] }>(a, '/runs').then((r) => r.runs),
  runDetail: (a: Args, id: string) =>
    _request<RunDetail>(a, `/runs/${encodeURIComponent(id)}`),
  backtests: (a: Args) => _request<BacktestQueue>(a, '/backtests'),
  launchBacktest: (a: Args, body: LaunchBacktestBody) =>
    _request<LaunchBacktestResponse>(a, '/backtests', {
      method: 'POST',
      body: JSON.stringify(body),
    }),
  kill: (a: Args) =>
    _request<SignalResponse>(a, '/kill', { method: 'POST' }),
  liquidate: (a: Args) =>
    _request<SignalResponse>(a, '/liquidate', { method: 'POST' }),
  databentoCredits: (a: Args) =>
    _request<DatabentoCreditsResponse>(a, '/databento/credits'),
};
