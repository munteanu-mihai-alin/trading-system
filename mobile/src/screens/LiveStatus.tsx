// Home screen. Polls /live/status every 10s, shows process state +
// RSS + elapsed + log tail. Two big destructive buttons at the
// bottom: SIGUSR1 (freeze trader) and SIGUSR2 (force-liquidate).
//
// Long-press confirm on the destructive buttons so a thumb-flick
// can't accidentally freeze the engine at the wrong time.

import React, { useCallback, useEffect, useState } from 'react';
import {
  Alert,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { api, AuthError } from '@/api/client';
import {
  Card,
  ErrorView,
  Loading,
  PrimaryButton,
  Stat,
  colors,
} from '@/components/primitives';
import { useAuth } from '@/state/AuthContext';
import type { LiveStatusResponse } from '@/api/types';

const POLL_MS = 10_000;

export const LiveStatusScreen: React.FC = () => {
  const auth = useAuth();
  const [data, setData] = useState<LiveStatusResponse | null>(null);
  const [err, setErr] = useState<unknown>(null);
  const [refreshing, setRefreshing] = useState(false);

  const args = { baseUrl: auth.baseUrl!, token: auth.token! };

  const load = useCallback(async () => {
    try {
      const d = await api.liveStatus(args);
      setData(d);
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
    const id = setInterval(load, POLL_MS);
    return () => clearInterval(id);
  }, [load]);

  const onRefresh = useCallback(async () => {
    setRefreshing(true);
    await load();
    setRefreshing(false);
  }, [load]);

  const confirmAndSend = (
    title: string,
    body: string,
    fn: () => Promise<unknown>,
  ) =>
    Alert.alert(title, body, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Confirm',
        style: 'destructive',
        onPress: async () => {
          try {
            const r = (await fn()) as { sent_to?: string[]; reason?: string };
            if (r.sent_to && r.sent_to.length > 0) {
              Alert.alert('Signal sent', `Delivered to pid ${r.sent_to[0]}`);
            } else {
              Alert.alert('No-op', r.reason ?? 'no hft_app running');
            }
            // Refresh immediately so the cards reflect the new state.
            load();
          } catch (e) {
            Alert.alert(
              'Failed',
              e instanceof Error ? e.message : String(e),
            );
          }
        },
      },
    ]);

  if (err) return <ErrorView error={err} onRetry={load} />;
  if (!data) return <Loading label="Loading status…" />;

  const p = data.process;
  const stateTone: 'good' | 'bad' = p.running ? 'good' : 'bad';

  return (
    <SafeAreaView style={styles.root}>
      <ScrollView
        contentContainerStyle={styles.scroll}
        refreshControl={
          <RefreshControl
            refreshing={refreshing}
            onRefresh={onRefresh}
            tintColor={colors.accent}
          />
        }>
        <Card title="Engine">
          <Stat
            label="State"
            value={p.running ? 'running' : 'stopped'}
            tone={stateTone}
          />
          <Stat label="PID" value={p.pid} />
          <Stat label="RSS (MB)" value={p.rss_mb} />
          <Stat label="Elapsed" value={p.elapsed} />
        </Card>

        <Card title="Log tail">
          {data.log_tail.length === 0 ? (
            <Text style={styles.dim}>(no log lines)</Text>
          ) : (
            <View>
              {data.log_tail.slice(-12).map((line, i) => (
                <Text key={i} style={styles.logLine} numberOfLines={2}>
                  {line}
                </Text>
              ))}
            </View>
          )}
        </Card>

        <View style={{ height: 14 }} />
        <PrimaryButton
          label="Freeze trader  (SIGUSR1)"
          destructive
          onPress={() =>
            confirmAndSend(
              'Freeze trader?',
              'Cancels every open entry + exit. Refuses new orders. Open positions stay open.',
              () => api.kill(args),
            )
          }
        />
        <View style={{ height: 10 }} />
        <PrimaryButton
          label="Force liquidate  (SIGUSR2)"
          destructive
          onPress={() =>
            confirmAndSend(
              'Force liquidate?',
              'Freeze trader PLUS post marketable sells at best_bid for every open position. Use when holding is riskier than the immediate exit.',
              () => api.liquidate(args),
            )
          }
        />
      </ScrollView>
    </SafeAreaView>
  );
};

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg },
  scroll: { padding: 14, paddingBottom: 40 },
  dim: { color: colors.textDim, fontSize: 13 },
  logLine: {
    color: colors.text,
    fontSize: 11,
    fontFamily: 'monospace',
    lineHeight: 15,
  },
});
