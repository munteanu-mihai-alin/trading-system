// Navigation entry point. AuthProvider gates between the Login
// screen (unauthenticated) and the tab navigator (authenticated).
// A logout button lives in the Live tab header.

import React from 'react';
import { TouchableOpacity, Text, View } from 'react-native';
import { NavigationContainer, DefaultTheme } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { createBottomTabNavigator } from '@react-navigation/bottom-tabs';
import { Ionicons } from '@expo/vector-icons';

import { useAuth } from '@/state/AuthContext';
import { Loading, colors } from '@/components/primitives';
import { LoginScreen } from '@/screens/Login';
import { LiveStatusScreen } from '@/screens/LiveStatus';
import { RunsListScreen } from '@/screens/RunsList';
import { RunDetailScreen } from '@/screens/RunDetail';
import { LaunchBacktestScreen } from '@/screens/LaunchBacktest';
import type {
  RootTabParamList,
  RunsStackParamList,
} from '@/navigation/types';

const Tab = createBottomTabNavigator<RootTabParamList>();
const RunsStack = createNativeStackNavigator<RunsStackParamList>();

const navTheme = {
  ...DefaultTheme,
  dark: true,
  colors: {
    ...DefaultTheme.colors,
    background: colors.bg,
    card: colors.card,
    border: colors.border,
    text: colors.text,
    primary: colors.accent,
    notification: colors.warn,
  },
};

const RunsStackScreen: React.FC = () => (
  <RunsStack.Navigator
    screenOptions={{
      headerStyle: { backgroundColor: colors.card },
      headerTintColor: colors.text,
    }}>
    <RunsStack.Screen
      name="RunsList"
      component={RunsListScreen}
      options={{ title: 'Runs' }}
    />
    <RunsStack.Screen
      name="RunDetail"
      component={RunDetailScreen}
      options={({ route }) => ({ title: route.params.id.slice(0, 20) })}
    />
  </RunsStack.Navigator>
);

const TabNav: React.FC = () => {
  const auth = useAuth();
  return (
    <Tab.Navigator
      screenOptions={{
        tabBarActiveTintColor: colors.accent,
        tabBarInactiveTintColor: colors.textDim,
        tabBarStyle: {
          backgroundColor: colors.card,
          borderTopColor: colors.border,
        },
        headerStyle: { backgroundColor: colors.card },
        headerTintColor: colors.text,
      }}>
      <Tab.Screen
        name="Live"
        component={LiveStatusScreen}
        options={{
          title: 'Live',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="pulse" color={color} size={size} />
          ),
          headerRight: () => (
            <TouchableOpacity
              onPress={() => auth.clear()}
              style={{ paddingHorizontal: 12 }}>
              <Text style={{ color: colors.textDim, fontSize: 13 }}>
                Sign out
              </Text>
            </TouchableOpacity>
          ),
        }}
      />
      <Tab.Screen
        name="Runs"
        component={RunsStackScreen}
        options={{
          headerShown: false,
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="list" color={color} size={size} />
          ),
        }}
      />
      <Tab.Screen
        name="Launch"
        component={LaunchBacktestScreen}
        options={{
          title: 'Launch',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="rocket" color={color} size={size} />
          ),
        }}
      />
    </Tab.Navigator>
  );
};

export const RootNavigator: React.FC = () => {
  const auth = useAuth();
  if (!auth.ready) {
    return (
      <View style={{ flex: 1, backgroundColor: colors.bg }}>
        <Loading label="Reading secure storage…" />
      </View>
    );
  }
  return (
    <NavigationContainer theme={navTheme}>
      {auth.baseUrl && auth.token ? <TabNav /> : <LoginScreen />}
    </NavigationContainer>
  );
};
