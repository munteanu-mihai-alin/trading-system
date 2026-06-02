// First screen the operator hits. Two text inputs (backend URL +
// bearer token), a "test connection" button that calls /health, and
// a save button that drops the values into SecureStore. After save
// the AuthProvider re-renders the tree into the tab navigator.

import React, { useState } from 'react';
import {
  Alert,
  KeyboardAvoidingView,
  Platform,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { api, ApiError, AuthError } from '@/api/client';
import { colors, PrimaryButton } from '@/components/primitives';
import { useAuth } from '@/state/AuthContext';

export const LoginScreen: React.FC = () => {
  const auth = useAuth();
  const [baseUrl, setBaseUrl] = useState(auth.baseUrl ?? '');
  const [token, setToken] = useState(auth.token ?? '');
  const [testing, setTesting] = useState(false);
  const [testResult, setTestResult] = useState<string | null>(null);

  const test = async () => {
    if (!baseUrl || !token) {
      Alert.alert('Missing', 'Need both backend URL and token');
      return;
    }
    setTesting(true);
    setTestResult(null);
    try {
      const cleanUrl = baseUrl.trim().replace(/\/$/, '');
      const h = await api.health({ baseUrl: cleanUrl, token: token.trim() });
      setTestResult(
        `OK: backend v${h.version}, /health returned ok=${h.ok}`,
      );
    } catch (e) {
      if (e instanceof AuthError) {
        setTestResult('Auth failed: token rejected by backend');
      } else if (e instanceof ApiError) {
        setTestResult(`HTTP ${e.status}: ${e.body.slice(0, 120)}`);
      } else {
        setTestResult(
          `Network error: ${e instanceof Error ? e.message : String(e)}`,
        );
      }
    } finally {
      setTesting(false);
    }
  };

  const save = async () => {
    if (!baseUrl || !token) {
      Alert.alert('Missing', 'Need both backend URL and token');
      return;
    }
    await auth.save(baseUrl, token);
  };

  return (
    <SafeAreaView style={styles.root}>
      <KeyboardAvoidingView
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
        style={{ flex: 1 }}>
        <ScrollView contentContainerStyle={styles.scroll}>
          <Text style={styles.title}>HFT Console</Text>
          <Text style={styles.subtitle}>
            Connect to the backend on Hetzner. Reach it via wireguard
            or an SSH tunnel; the API is not exposed publicly.
          </Text>

          <Text style={styles.label}>Backend URL</Text>
          <TextInput
            value={baseUrl}
            onChangeText={setBaseUrl}
            placeholder="http://10.66.66.1:8088"
            placeholderTextColor={colors.textDim}
            autoCapitalize="none"
            autoCorrect={false}
            keyboardType="url"
            style={styles.input}
          />

          <Text style={styles.label}>Bearer token</Text>
          <Text style={styles.hint}>
            From <Text style={styles.code}>/etc/hft/api.env</Text>'s{' '}
            <Text style={styles.code}>API_TOKEN</Text>.
          </Text>
          <TextInput
            value={token}
            onChangeText={setToken}
            placeholder="paste API_TOKEN here"
            placeholderTextColor={colors.textDim}
            autoCapitalize="none"
            autoCorrect={false}
            secureTextEntry
            style={styles.input}
          />

          <View style={{ marginTop: 16 }}>
            <PrimaryButton
              label={testing ? 'Testing…' : 'Test connection'}
              onPress={test}
              disabled={testing}
            />
          </View>
          {testResult ? (
            <Text
              style={[
                styles.result,
                testResult.startsWith('OK')
                  ? { color: colors.good }
                  : { color: colors.bad },
              ]}>
              {testResult}
            </Text>
          ) : null}

          <View style={{ marginTop: 24 }}>
            <PrimaryButton label="Save and continue" onPress={save} />
          </View>
        </ScrollView>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
};

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg },
  scroll: { padding: 18 },
  title: {
    color: colors.text,
    fontSize: 28,
    fontWeight: '800',
    marginBottom: 4,
  },
  subtitle: {
    color: colors.textDim,
    fontSize: 14,
    marginBottom: 24,
    lineHeight: 20,
  },
  label: {
    color: colors.text,
    fontSize: 13,
    marginTop: 14,
    marginBottom: 4,
    fontWeight: '600',
  },
  hint: {
    color: colors.textDim,
    fontSize: 12,
    marginBottom: 6,
  },
  code: {
    fontFamily: Platform.OS === 'ios' ? 'Menlo' : 'monospace',
    color: colors.accent,
  },
  input: {
    backgroundColor: colors.card,
    borderColor: colors.border,
    borderWidth: 1,
    color: colors.text,
    paddingHorizontal: 12,
    paddingVertical: 10,
    borderRadius: 8,
    fontSize: 15,
  },
  result: {
    marginTop: 8,
    fontSize: 13,
    lineHeight: 18,
  },
});
