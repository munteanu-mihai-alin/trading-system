# HFT Console (mobile)

React Native + Expo + TypeScript. Talks to the FastAPI backend on
Hetzner (`scripts/backend/api.py`). Five screens:

| Screen | What |
|---|---|
| Login | Backend URL + bearer token (stored in SecureStore) |
| Live | Polls `/live/status`; freeze + liquidate buttons |
| Runs | List of historical backtests with headline PnL / win rate |
| Run detail | metrics.json + orders.csv head |
| Launch | Preset-driven `POST /backtests` |

## Prerequisites

- Node.js 20+
- A phone with **Expo Go** installed (App Store / Play Store), OR
  an iOS simulator (Xcode) / Android emulator (Android Studio).
- The Hetzner backend reachable from your phone:
  - Recommended: **WireGuard** tunnel from the phone, then
    Backend URL = `http://10.66.66.1:8088`.
  - Alternative: SSH port forward via Termius / Prompt 3.

## Dev workflow

```bash
cd mobile
npm install
npm start                    # starts Expo, prints a QR code
```

Scan the QR code from Expo Go on your phone. The app hot-reloads
on save.

For an emulator instead of your phone:

```bash
npm run ios                  # iOS simulator (macOS only, needs Xcode)
npm run android              # Android emulator (Android Studio)
```

## First launch

1. Set up wireguard on your phone if you haven't.
2. Make sure the backend is up:
   `ssh hetzner 'systemctl status hft_backend.service'`
3. Open the app. Login screen.
4. Backend URL: your tunneled `http://...:8088`.
5. Token: from `/etc/hft/api.env`'s `API_TOKEN` on Hetzner.
6. Tap **Test connection** — expects "OK: backend vX.Y, /health
   returned ok=true".
7. Save and continue.

## Producing a real app build

For day-to-day operator use you want the app on the home screen,
not running through Expo Go. Two paths:

### EAS Build (managed, recommended)

```bash
npm install -g eas-cli
eas login
eas build --platform android --profile preview      # APK
eas build --platform ios --profile preview          # IPA (needs Apple dev acct)
```

Output: a download URL for the artifact. Sideload the APK with
`adb install hft-console.apk`, or distribute via TestFlight for iOS.

### Local build (Android)

If you have the Android SDK already:
```bash
npx expo prebuild --platform android
cd android
./gradlew assembleRelease
```
APK lands in `android/app/build/outputs/apk/release/`.

## Architecture

```
App.tsx
├─ AuthProvider               (state/AuthContext.tsx)
│    SecureStore for baseUrl + token
└─ RootNavigator              (navigation/RootNavigator.tsx)
     - unauthenticated  -> LoginScreen
     - authenticated    -> Bottom tabs:
                            Live / Runs / Launch
```

All HTTP goes through `src/api/client.ts` which injects the
`X-HFT-Token` header and routes 401 -> auto-logout.

## What's deferred to follow-ups

- WebView for the per-run `report.html` (today: metrics table only).
- Push notifications via FCM/APNS (today: notify.sh handles
  alerts through ntfy.sh / Telegram).
- Chat tab against `POST /chat` (backend stubbed 501).
- A "running backtest" tail view (today: launch + check Runs list
  later).
- Symbol-universe picker on the Launch form (today: preset only).
- Charts (today: numbers only).
