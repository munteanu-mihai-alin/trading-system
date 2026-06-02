// Light visual layer so the screens stay focused on data + behaviour.
// Color tokens follow the same dark-mode-by-default theme as the
// HTML report (report.html in plot_run.py uses very similar values).

import React from 'react';
import {
  ActivityIndicator,
  StyleSheet,
  Text,
  TextStyle,
  TouchableOpacity,
  View,
  ViewStyle,
} from 'react-native';

export const colors = {
  bg: '#0d1117',
  card: '#161b22',
  border: '#30363d',
  text: '#c9d1d9',
  textDim: '#8b949e',
  good: '#3fb950',
  bad: '#f85149',
  warn: '#d29922',
  accent: '#58a6ff',
};

export const Card: React.FC<{
  title?: string;
  children: React.ReactNode;
  style?: ViewStyle;
}> = ({ title, children, style }) => (
  <View style={[styles.card, style]}>
    {title ? <Text style={styles.cardTitle}>{title}</Text> : null}
    {children}
  </View>
);

export const Stat: React.FC<{
  label: string;
  value: string | number | null | undefined;
  tone?: 'good' | 'bad' | 'warn' | 'normal';
}> = ({ label, value, tone = 'normal' }) => {
  const valueColor =
    tone === 'good'
      ? colors.good
      : tone === 'bad'
        ? colors.bad
        : tone === 'warn'
          ? colors.warn
          : colors.text;
  return (
    <View style={styles.stat}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={[styles.statValue, { color: valueColor }]}>
        {value === null || value === undefined ? '—' : String(value)}
      </Text>
    </View>
  );
};

export const PrimaryButton: React.FC<{
  label: string;
  onPress: () => void;
  destructive?: boolean;
  disabled?: boolean;
}> = ({ label, onPress, destructive, disabled }) => (
  <TouchableOpacity
    style={[
      styles.button,
      destructive && { backgroundColor: colors.bad, borderColor: colors.bad },
      disabled && { opacity: 0.4 },
    ]}
    onPress={disabled ? undefined : onPress}
    activeOpacity={0.7}>
    <Text style={styles.buttonText}>{label}</Text>
  </TouchableOpacity>
);

export const Loading: React.FC<{ label?: string }> = ({ label }) => (
  <View style={styles.center}>
    <ActivityIndicator color={colors.accent} />
    {label ? (
      <Text style={[styles.dim, { marginTop: 8 }]}>{label}</Text>
    ) : null}
  </View>
);

export const ErrorView: React.FC<{
  error: unknown;
  onRetry?: () => void;
}> = ({ error, onRetry }) => {
  const msg = error instanceof Error ? error.message : String(error);
  return (
    <View style={styles.center}>
      <Text style={{ color: colors.bad, textAlign: 'center' }}>{msg}</Text>
      {onRetry ? (
        <View style={{ marginTop: 12 }}>
          <PrimaryButton label="Retry" onPress={onRetry} />
        </View>
      ) : null}
    </View>
  );
};

const styles = StyleSheet.create({
  card: {
    backgroundColor: colors.card,
    borderColor: colors.border,
    borderWidth: 1,
    borderRadius: 12,
    padding: 14,
    marginVertical: 6,
  },
  cardTitle: {
    color: colors.textDim,
    fontSize: 12,
    fontWeight: '600',
    textTransform: 'uppercase',
    letterSpacing: 0.7,
    marginBottom: 8,
  },
  stat: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    paddingVertical: 4,
  },
  statLabel: {
    color: colors.textDim,
    fontSize: 14,
  },
  statValue: {
    color: colors.text,
    fontSize: 14,
    fontWeight: '600',
  },
  button: {
    backgroundColor: colors.accent,
    borderColor: colors.accent,
    borderWidth: 1,
    paddingHorizontal: 18,
    paddingVertical: 12,
    borderRadius: 10,
    alignItems: 'center',
  },
  buttonText: {
    color: '#0d1117',
    fontWeight: '700',
    fontSize: 15,
  },
  center: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    padding: 16,
  },
  dim: {
    color: colors.textDim,
    fontSize: 13,
  } as TextStyle,
});

export { styles as primStyles };
