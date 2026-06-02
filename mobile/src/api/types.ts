// Type definitions mirror scripts/backend/api.py response shapes.
// Loose where the backend itself returns Dict[str, Any] (metrics,
// launcher state) - those are the messy bits where we want to keep
// the schema flexible without churning the app.

export interface HealthResponse {
  ok: boolean;
  version: string;
}

export interface RunSummary {
  id: string;
  n_round_trips_closed: number | null;
  realized_pnl_net: number | null;
  win_rate: number | null;
  sharpe_ratio_annualized: number | null;
  avg_holding_minutes: number | null;
  has_metrics: boolean;
}

export interface RunDetail {
  id: string;
  metrics: Record<string, unknown>;
  orders_head: string[];
  has_report_html: boolean;
}

export interface LiveStatusProcess {
  running: boolean;
  pid: number | null;
  rss_mb: number | null;
  elapsed: string | null;
}

export interface LiveStatusResponse {
  process: LiveStatusProcess;
  log_tail: string[];
}

export interface BacktestQueue {
  queued: string[];
  running: string[];
  done: string[];
  launcher: Record<string, unknown>;
}

export interface LaunchBacktestBody {
  config: string;
  label?: string;
  target_profit_pct?: number;
  start?: string;
  end?: string;
  symbols?: string;
  binary_version?: string;
}

export interface LaunchBacktestResponse {
  id: string;
  queued_at: string;
  queue_path: string;
}

export interface SignalResponse {
  sent_to: string[];
  signal?: string;
  reason?: string;
}

export interface DatabentoCreditsResponse {
  available: boolean;
  raw?: unknown;
  reason?: string;
  manual_url?: string;
}
