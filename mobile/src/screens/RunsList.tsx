// Backtest runs list. Pull-to-refresh; tap a row to drill into
// RunDetail. The headline numbers come straight from metrics.json,
// computed by scripts/plot_run.py after each backtest archives.

import React, { useCallback, useEffect, useState } from 'react';
import {
  FlatList,
  RefreshControl,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';

import { api, AuthError } from '@/api/client';
import { ErrorView, Loading, colors } from '@/components/primitives';
import { useAuth } from '@/state/AuthContext';
import type { RunsStackParamList } from '@/navigation/types';
import type { RunSummary } from '@/api/types';

type Props = NativeStackScreenProps<RunsStackParamList, 'RunsList'>;

export const RunsListScreen: React.FC<Props> = ({ navigation }) => {
  const auth = useAuth();
  const [runs, setRuns] = useState<RunSummary[] | null>(null);
  const [err, setErr] = useState<unknown>(null);
  const [refreshing, setRefreshing] = useState(false);

  const args = { baseUrl: auth.baseUrl!, token: auth.token! };

  const load = useCallback(async () => {
    try {
      setRuns(await api.runs(args));
      setErr(null);
    } catch (e) {
      if (e instanceof AuthError) {
        await auth.clear();
        return;
      }
      setErr(e);
    }
  }, [args.baseUrl, args.token]);

  useEffect(() => {
    load();
  }, [load]);

  const onRefresh = useCallback(async () => {
    setRefreshing(true);
    await load();
    setRefreshing(false);
  }, [load]);

  if (err) return <ErrorView error={err} onRetry={load} />;
  if (!runs) return <Loading label="Loading runs…" />;

  return (
    <SafeAreaView style={styles.root} edges={['left', 'right']}>
      <FlatList
        data={runs}
        keyExtractor={(r) => r.id}
        contentContainerStyle={runs.length === 0 ? styles.empty : undefined}
        refreshControl={
          <RefreshControl
            refreshing={refreshing}
            onRefresh={onRefresh}
            tintColor={colors.accent}
          />
        }
        ListEmptyComponent={
          <Text style={styles.dim}>no runs found in reports/runs/</Text>
        }
        renderItem={({ item }) => (
          <TouchableOpacity
            style={styles.row}
            onPress={() => navigation.navigate('RunDetail', { id: item.id })}
            activeOpacity={0.7}>
            <View style={{ flex: 1 }}>
              <Text style={styles.runId} numberOfLines={1}>
                {item.id}
              </Text>
              <Text style={styles.dim}>
                {item.n_round_trips_closed ?? '—'} trips ·{' '}
                {item.avg_holding_minutes != null
                  ? `${Math.round(item.avg_holding_minutes)} min hold`
                  : '— hold'}
              </Text>
            </View>
            <View style={styles.metricsCol}>
              <Text
                style={[
                  styles.pnl,
                  {
                    color:
                      (item.realized_pnl_net ?? 0) > 0
                        ? colors.good
                        : (item.realized_pnl_net ?? 0) < 0
                          ? colors.bad
                          : colors.textDim,
                  },
                ]}>
                {item.realized_pnl_net != null
                  ? `${item.realized_pnl_net.toFixed(2)}$`
                  : '—'}
              </Text>
              <Text style={styles.dim}>
                {item.win_rate != null
                  ? `${(item.win_rate * 100).toFixed(0)}%`
                  : '—'}{' '}
                wr
              </Text>
            </View>
          </TouchableOpacity>
        )}
      />
    </SafeAreaView>
  );
};

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg },
  empty: {
    flex: 1,
    justifyContent: 'center',
    alignItems: 'center',
    padding: 16,
  },
  row: {
    flexDirection: 'row',
    backgroundColor: colors.card,
    borderColor: colors.border,
    borderBottomWidth: 1,
    paddingHorizontal: 14,
    paddingVertical: 14,
  },
  runId: { color: colors.text, fontSize: 13, fontWeight: '600' },
  metricsCol: { alignItems: 'flex-end' },
  pnl: { fontSize: 15, fontWeight: '700' },
  dim: { color: colors.textDim, fontSize: 12, marginTop: 2 },
});
