// Per-run detail. Shows the metrics.json key/value table (after
// hoisting the headline metrics to the top) and the head of
// orders.csv. The full HTML report is reachable when
// has_report_html=true, but rendering it via WebView is deferred to
// a follow-up - in the meantime the table + orders preview is
// enough to answer "did this run make money".

import React, { useCallback, useEffect, useState } from 'react';
import {
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';

import { api, AuthError } from '@/api/client';
import {
  Card,
  ErrorView,
  Loading,
  Stat,
  colors,
} from '@/components/primitives';
import { useAuth } from '@/state/AuthContext';
import type { RunsStackParamList } from '@/navigation/types';
import type { RunDetail } from '@/api/types';

type Props = NativeStackScreenProps<RunsStackParamList, 'RunDetail'>;

const HEADLINE_KEYS = [
  'n_round_trips_closed',
  'n_positions_open_at_end',
  'realized_pnl_net',
  'unrealized_pnl_mark_to_market',
  'net_pnl_after_opportunity_cost',
  'win_rate',
  'sharpe_ratio_annualized',
  'sortino_ratio_annualized',
  'max_drawdown_dollars',
  'avg_holding_minutes',
] as const;

const fmt = (v: unknown): string => {
  if (v == null) return '—';
  if (typeof v === 'number') {
    if (Math.abs(v) < 0.01 && v !== 0) return v.toExponential(2);
    return v.toFixed(4);
  }
  return String(v);
};

export const RunDetailScreen: React.FC<Props> = ({ route }) => {
  const auth = useAuth();
  const { id } = route.params;
  const [data, setData] = useState<RunDetail | null>(null);
  const [err, setErr] = useState<unknown>(null);
  const [refreshing, setRefreshing] = useState(false);

  const args = { baseUrl: auth.baseUrl!, token: auth.token! };

  const load = useCallback(async () => {
    try {
      setData(await api.runDetail(args, id));
      setErr(null);
    } catch (e) {
      if (e instanceof AuthError) {
        await auth.clear();
        return;
      }
      setErr(e);
    }
  }, [args.baseUrl, args.token, id]);

  useEffect(() => {
    load();
  }, [load]);

  const onRefresh = useCallback(async () => {
    setRefreshing(true);
    await load();
    setRefreshing(false);
  }, [load]);

  if (err) return <ErrorView error={err} onRetry={load} />;
  if (!data) return <Loading label="Loading run…" />;

  const metrics = data.metrics ?? {};
  const realized = (metrics as Record<string, number>).realized_pnl_net;
  const realizedTone: 'good' | 'bad' | 'normal' =
    realized > 0 ? 'good' : realized < 0 ? 'bad' : 'normal';

  return (
    <SafeAreaView style={styles.root} edges={['left', 'right']}>
      <ScrollView
        contentContainerStyle={styles.scroll}
        refreshControl={
          <RefreshControl
            refreshing={refreshing}
            onRefresh={onRefresh}
            tintColor={colors.accent}
          />
        }>
        <Text style={styles.runId}>{id}</Text>

        <Card title="Headline metrics">
          {HEADLINE_KEYS.map((k) => {
            const v = (metrics as Record<string, unknown>)[k];
            const tone =
              k === 'realized_pnl_net' || k === 'net_pnl_after_opportunity_cost'
                ? realizedTone
                : 'normal';
            return <Stat key={k} label={k} value={fmt(v)} tone={tone} />;
          })}
        </Card>

        <Card title="Orders (head 50)">
          {data.orders_head.length === 0 ? (
            <Text style={styles.dim}>no orders.csv content</Text>
          ) : (
            data.orders_head.slice(0, 50).map((line, i) => (
              <Text key={i} style={styles.csvLine} numberOfLines={1}>
                {line}
              </Text>
            ))
          )}
        </Card>

        {data.has_report_html ? (
          <Text style={styles.dim}>
            (full report.html exists on the server; viewer in a follow-up)
          </Text>
        ) : null}
      </ScrollView>
    </SafeAreaView>
  );
};

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg },
  scroll: { padding: 14, paddingBottom: 40 },
  runId: {
    color: colors.text,
    fontSize: 16,
    fontWeight: '700',
    marginBottom: 12,
  },
  dim: { color: colors.textDim, fontSize: 12, marginTop: 6 },
  csvLine: {
    color: colors.text,
    fontSize: 10,
    fontFamily: 'monospace',
    lineHeight: 14,
  },
});
