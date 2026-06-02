// One-tap backtest launcher. Preset selects the config file + a
// sensible default for the period; the operator can override
// target_profit_pct and label inline. Hits POST /backtests which
// drops a job into queue/incoming/ for the launcher daemon.

import React, { useState } from 'react';
import {
  Alert,
  KeyboardAvoidingView,
  Platform,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { api } from '@/api/client';
import {
  Card,
  PrimaryButton,
  colors,
  primStyles,
} from '@/components/primitives';
import { useAuth } from '@/state/AuthContext';

interface Preset {
  id: string;
  label: string;
  config: string;
  start?: string;
  end?: string;
}

const PRESETS: Preset[] = [
  {
    id: 'yen',
    label: 'Yen (Aug 2024)',
    config: 'config.databento_backtest.yen.ini',
    start: '2024-08-02T13:30:00Z',
    end: '2024-08-09T20:00:00Z',
  },
  {
    id: 'covid',
    label: 'COVID (Mar 2020)',
    config: 'config.databento_backtest.covid.ini',
    start: '2020-03-09T13:30:00Z',
    end: '2020-03-20T20:00:00Z',
  },
  {
    id: '10day',
    label: '10-day baseline',
    config: 'config.databento_backtest.example.ini',
  },
];

export const LaunchBacktestScreen: React.FC = () => {
  const auth = useAuth();
  const [preset, setPreset] = useState<Preset>(PRESETS[0]);
  const [label, setLabel] = useState('');
  const [target, setTarget] = useState('0.008');
  const [submitting, setSubmitting] = useState(false);

  const args = { baseUrl: auth.baseUrl!, token: auth.token! };

  const submit = async () => {
    if (submitting) return;
    setSubmitting(true);
    try {
      const body = {
        config: preset.config,
        label: label || `${preset.id}_${Math.floor(Date.now() / 1000)}`,
        target_profit_pct: target ? Number(target) : undefined,
        start: preset.start,
        end: preset.end,
      };
      const r = await api.launchBacktest(args, body);
      Alert.alert(
        'Queued',
        `Job ${r.id} dropped at ${r.queued_at}.\nLauncher will pick it up.`,
      );
      setLabel('');
    } catch (e) {
      Alert.alert(
        'Launch failed',
        e instanceof Error ? e.message : String(e),
      );
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <SafeAreaView style={styles.root} edges={['left', 'right']}>
      <KeyboardAvoidingView
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
        style={{ flex: 1 }}>
        <ScrollView contentContainerStyle={styles.scroll}>
          <Card title="Preset">
            <View style={styles.presetRow}>
              {PRESETS.map((p) => (
                <TouchableOpacity
                  key={p.id}
                  style={[
                    styles.presetChip,
                    preset.id === p.id && styles.presetChipActive,
                  ]}
                  onPress={() => setPreset(p)}
                  activeOpacity={0.7}>
                  <Text
                    style={[
                      styles.presetChipText,
                      preset.id === p.id && {
                        color: '#0d1117',
                        fontWeight: '700',
                      },
                    ]}>
                    {p.label}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
            <Text style={styles.dim}>config: {preset.config}</Text>
            {preset.start ? (
              <Text style={styles.dim}>
                {preset.start} → {preset.end}
              </Text>
            ) : null}
          </Card>

          <Card title="Overrides">
            <Text style={styles.label}>Label (defaults to preset+ts)</Text>
            <TextInput
              value={label}
              onChangeText={setLabel}
              placeholder={`${preset.id}_v…`}
              placeholderTextColor={colors.textDim}
              autoCapitalize="none"
              style={styles.input}
            />

            <Text style={[styles.label, { marginTop: 12 }]}>
              target_profit_pct
            </Text>
            <TextInput
              value={target}
              onChangeText={setTarget}
              placeholder="0.008"
              placeholderTextColor={colors.textDim}
              keyboardType="decimal-pad"
              style={styles.input}
            />
          </Card>

          <View style={{ marginTop: 14 }}>
            <PrimaryButton
              label={submitting ? 'Queueing…' : 'Queue backtest'}
              onPress={submit}
              disabled={submitting}
            />
          </View>
          <Text style={[styles.dim, { marginTop: 14, textAlign: 'center' }]}>
            Backend writes the job file to queue/incoming/.{'\n'}
            The launcher daemon picks it up within {`<`}5s.
          </Text>
        </ScrollView>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
};

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg },
  scroll: { padding: 14, paddingBottom: 40 },
  presetRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 6,
  },
  presetChip: {
    paddingHorizontal: 12,
    paddingVertical: 8,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: colors.border,
    backgroundColor: colors.bg,
  },
  presetChipActive: {
    backgroundColor: colors.accent,
    borderColor: colors.accent,
  },
  presetChipText: { color: colors.text, fontSize: 13 },
  dim: { color: colors.textDim, fontSize: 12, marginTop: 6 },
  label: {
    color: colors.text,
    fontSize: 13,
    fontWeight: '600',
    marginBottom: 4,
  },
  input: {
    ...primStyles.card,
    color: colors.text,
    paddingHorizontal: 10,
    paddingVertical: 8,
    marginVertical: 0,
    fontSize: 14,
  },
});
