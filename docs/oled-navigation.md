# OLED screen navigation

Applies to the `esp32-s3-dev-qr-onboarding-demo` and `esp32-s3-dev-local-oled-demo`
PlatformIO environments (the only two that define `ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS`).
The local-oled-demo profile has no QR pairing, so it has no "Pair Device" item
or Physical-Unpair overlay -- both are specific to `ALGAGUARD_ENABLE_QR_ONBOARDING`.

## Buttons

Four hardware buttons, active-low, 35ms debounce, short-press only:

| Button | GPIO | Role |
|---|---|---|
| Up | 4 | Move the highlight up (main menu only) |
| Down | 5 | Move the highlight down (main menu only) |
| Select | 6 | Enter the highlighted screen / act |
| Back | 7 | Return to the main menu (context-dependent inside sub-flows) |

## Flowchart

```mermaid
flowchart TD
    Boot["Boot screen<br/>(~4s animated splash,<br/>auto-transitions)"]
    Menu["Main menu<br/>(scrollable list, highlighted row)"]

    Boot --> Menu

    Menu -- "Select: Pair Device *" --> PairPrompt
    Menu -- "Select: Temp and pH" --> TempPh["Temperature / pH<br/>(live values)"]
    Menu -- "Select: Light" --> Light["Light<br/>(live value)"]
    Menu -- "Select: Nutrients" --> Nutrients["Nutrients (N/P/K)<br/>(live values)"]
    Menu -- "Select: Device Status" --> DeviceStatus["Device Status<br/>(BLE/WiFi/cloud state)"]
    Menu -- "Select: Network" --> NetReady
    Menu -- "Select: About" --> About["About<br/>(firmware version)"]

    TempPh -- Back --> Menu
    Light -- Back --> Menu
    Nutrients -- Back --> Menu
    DeviceStatus -- Back --> Menu
    About -- Back --> Menu

    subgraph PairFlow ["Pair Device * (QR onboarding only)"]
        PairPrompt["Scan to add<br/>(prompt)"]
        PairCode["QR code<br/>(one-time)"]
        PairExpired["QR expired /<br/>used / error"]
        PairPrompt -- Select: show code --> PairCode
        PairCode -- Select: regenerate --> PairCode
        PairPrompt -. "auto: expires/used/error" .-> PairExpired
        PairCode -. "auto: expires/used/error" .-> PairExpired
        PairExpired -- Select: new QR --> PairPrompt
    end
    PairFlow -- Back --> Menu

    subgraph NetFlow ["Network"]
        NetReady["Network<br/>(WiFi/storage state)"]
        NetConfirm["Forget WiFi?<br/>(confirm)"]
        NetResult["WiFi forgotten /<br/>forget failed"]
        NetReady -- "Select: forget" --> NetConfirm
        NetConfirm -- "Select: confirm" --> NetResult
        NetConfirm -- "Back: cancel" --> NetReady
    end
    NetReady -- Back --> Menu
    NetResult -- Back --> Menu

    Unpair["Remove device?<br/>(confirmation overlay)"]
    Unpair -. "Select: confirm unpair,<br/>clears identity + WiFi" .-> Menu
    Unpair -- "Back: cancel" --> Unpair

    AnyScreen(["any screen"]) -. "remote REQUEST_PHYSICAL_UNPAIR<br/>command arrives (*)" .-> Unpair
```

`*` The Physical-Unpair overlay pre-empts *every* other screen the instant a
`REQUEST_PHYSICAL_UNPAIR` MQTT command arrives (only on the QR-onboarding
build, since it needs device identity/telemetry) -- Up/Down do nothing while
it's showing, Select confirms (wipes identity, clears WiFi, returns to the
main menu), Back cancels and returns to whatever was showing before.

## Screen-by-screen input reference

| Screen | Up / Down | Select | Back |
|---|---|---|---|
| Main menu | Move highlight (wraps) | Enter highlighted item | *(no-op, already at the top)* |
| Pair Device: prompt | -- | Show QR code | Return to main menu |
| Pair Device: QR code | -- | Regenerate a fresh one-time QR | Return to main menu |
| Pair Device: expired/used/error | -- | Generate a new QR (back to prompt flow) | Return to main menu |
| Temperature/pH, Light, Nutrients, Device Status, About | -- | -- | Return to main menu |
| Network: ready | -- | Ask to forget saved WiFi | Return to main menu |
| Network: confirm forget | -- | Actually forget WiFi | Cancel, stay on Network (ready) |
| Network: forgotten / failed | -- | Ask to forget again | Return to main menu |
| Physical-Unpair overlay | -- | Confirm unpair, return to main menu | Cancel, resume previous screen |

## Implementation notes

- `AppScreen` (`src/main.cpp`) is the single source of truth for "what's on
  screen" -- replaces the earlier `QrDisplayMode`/`LocalDemoPage`-as-navigation
  split. `PairDeviceSubMode` is the pairing flow's own prompt/code sub-state.
- `MainMenuNav` + `kMainMenuItems` (`src/main.cpp`) drive the scrollable menu;
  `compose_menu_screen()` (`include/algaguard/display.hpp`) renders it,
  including the highlighted-row invert and scroll-position triangles.
- `LocalDemoNetworkFlow` (`include/algaguard/local_demo.hpp`) tracks only the
  Network screen's forget-WiFi confirmation sub-state; `local_demo_screen()`
  still renders every content page's text unchanged.
- One `screen_revision` counter (`src/main.cpp`) gates redraws across the
  whole dispatch -- nothing is re-flushed to the panel unless something a
  visible screen actually depends on changed.
