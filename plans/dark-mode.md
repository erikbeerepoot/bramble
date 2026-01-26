# Dark Mode - Feature Plan

## Overview

Add a dark mode toggle to the Bramble Dashboard. The dashboard is a React + TypeScript + Tailwind CSS SPA that monitors a LoRa farm sensor network. Currently it only supports a light theme with a bramble-green header and light gray/white card backgrounds.

## Goals

- Allow users to switch between light and dark themes
- Persist the preference across sessions (localStorage)
- Respect the user's OS-level preference as default
- Ensure all components (cards, charts, modals, forms, status indicators) render correctly in both modes

## Current State

### Technology
- **Styling**: Tailwind CSS v3.3.6 (utility-first, supports `dark:` variant natively)
- **Custom colors**: `bramble` palette (50-900) in `tailwind.config.js`
- **Global styles**: Custom `.card`, `.btn`, `.btn-primary`, `.btn-secondary`, `.input` classes in `index.css`
- **Charts**: Plotly.js with transparent background, hardcoded `#f3f4f6` gridlines
- **Icons**: lucide-react with Tailwind color classes

### Files Requiring Changes
| File | Scope of Changes |
|------|-----------------|
| `tailwind.config.js` | Enable `darkMode: 'class'` strategy |
| `src/index.css` | Add `dark:` variants to `.card`, `.btn-*`, `.input`, body, Plotly hover styles |
| `src/App.tsx` | Add ThemeProvider context, toggle button in header, `dark` class on root `<div>` |
| `src/components/NodeCard.tsx` | `dark:` variants for card text, borders, status badges |
| `src/components/NodeList.tsx` | `dark:` variants for text, error cards, empty state |
| `src/components/NodeDetail.tsx` | `dark:` variants for text, stat cards, error states, borders |
| `src/components/NodeDetailPage.tsx` | `dark:` variants for loading/error states |
| `src/components/SensorChart.tsx` | Dynamic gridcolor, axis text color, hover label styling based on theme |
| `src/components/TimeRangeSelector.tsx` | `dark:` variants for button states, datetime inputs |
| `src/components/Settings.tsx` | `dark:` variants for text, test result banners |
| `src/components/NodeNameEditor.tsx` | `dark:` variants for text, form labels, edit mode |
| `src/components/CreateZoneModal.tsx` | `dark:` modal backdrop, panel bg, borders, form elements |
| `src/components/BatteryGauge.tsx` | Label text color variant |
| `src/components/SignalStrength.tsx` | Label text color variant |
| `src/components/HealthStatus.tsx` | Label text, tooltip bg (already dark, may need light-mode contrast fix) |
| `src/components/BacklogStatus.tsx` | No changes needed (uses color classes that work in both modes) |

## Design

### Approach: Tailwind `class` Strategy

Use Tailwind's `darkMode: 'class'` strategy. This toggles dark mode by adding/removing a `dark` class on a root element (`<html>` or root `<div>`). This is preferred over `media` strategy because it allows an explicit user toggle independent of OS setting.

### Theme Context

Create a `ThemeContext` in `App.tsx` (or a new `src/ThemeProvider.tsx` if cleaner) that:
1. On mount, reads from `localStorage.getItem('bramble-theme')`
2. Falls back to `window.matchMedia('(prefers-color-scheme: dark)')` if no stored preference
3. Applies/removes `dark` class on `document.documentElement`
4. Exposes `{ theme, toggleTheme }` via React context

### Toggle UI

Add a sun/moon icon button in the header nav bar (next to Nodes/Settings links). Use `Sun` and `Moon` icons from lucide-react (already a dependency).

### Dark Color Palette

| Element | Light | Dark |
|---------|-------|------|
| Page background | `bg-gray-50` | `dark:bg-gray-900` |
| Card background | `bg-white` | `dark:bg-gray-800` |
| Card border | `border-gray-200` | `dark:border-gray-700` |
| Primary text | `text-gray-900` | `dark:text-gray-100` |
| Secondary text | `text-gray-500`/`600`/`700` | `dark:text-gray-400`/`300` |
| Muted text | `text-gray-400` | `dark:text-gray-500` |
| Header | `bg-bramble-700` | `bg-bramble-800` (slightly darker) |
| Input border | `border-gray-300` | `dark:border-gray-600` |
| Input background | (default white) | `dark:bg-gray-700 dark:text-gray-100` |
| Hover card shadow | `hover:shadow-lg` | `dark:hover:shadow-lg dark:shadow-gray-900/30` |
| Status badge (online) | `bg-green-100 text-green-800` | `dark:bg-green-900 dark:text-green-300` |
| Status badge (offline) | `bg-red-100 text-red-800` | `dark:bg-red-900 dark:text-red-300` |
| Error banner | `bg-red-50 border-red-200 text-red-700` | `dark:bg-red-900/20 dark:border-red-800 dark:text-red-400` |
| Success banner | `bg-green-50 border-green-200 text-green-700` | `dark:bg-green-900/20 dark:border-green-800 dark:text-green-400` |
| Dividers | `border-gray-100`/`200` | `dark:border-gray-700` |
| Modal overlay | `bg-black bg-opacity-50` | same |
| Modal panel | `bg-white` | `dark:bg-gray-800` |
| Plotly gridlines | `#f3f4f6` | `#374151` (gray-700) |
| Plotly axis text | (default black) | white |
| Plotly hover tooltip | white bg | `dark:` dark bg via CSS |

### Plotly Chart Theming

Plotly doesn't use Tailwind classes, so the `SensorChart` component needs to read the current theme from context and pass appropriate colors to the layout config:
- `xaxis.gridcolor` / `yaxis.gridcolor`: `#f3f4f6` (light) vs `#374151` (dark)
- `xaxis.tickfont.color` / `yaxis.tickfont.color`: `#374151` (light) vs `#d1d5db` (dark)
- `yaxis.title.font.color`: same as tick font
- Hover label styling via CSS in `index.css` with `dark:` selector

## Implementation Steps

### Step 1: Tailwind Config & Global Styles
1. Add `darkMode: 'class'` to `tailwind.config.js`
2. Update `index.css`:
   - Body: add `dark:bg-gray-900 dark:text-gray-100`
   - `.card`: add `dark:bg-gray-800 dark:shadow-lg dark:shadow-gray-900/20`
   - `.btn-secondary`: add `dark:bg-gray-700 dark:text-gray-200 dark:hover:bg-gray-600`
   - `.input`: add `dark:bg-gray-700 dark:border-gray-600 dark:text-gray-100`
   - Plotly hover: add `.dark .hoverlayer ...` rule

### Step 2: Theme Context & Toggle
1. Create theme state management (context or simple hook in App.tsx)
2. Add localStorage persistence with `bramble-theme` key
3. Add OS preference detection as fallback
4. Apply `dark` class to `<html>` element
5. Add toggle button (Sun/Moon icon) to header in `App.tsx`

### Step 3: Update Layout Components
1. **App.tsx**: Header dark variant (`dark:bg-bramble-800`), nav link hover states
2. **NodeList.tsx**: Text colors, error/empty states, zone group headers
3. **NodeDetailPage.tsx**: Loading spinner text, error card

### Step 4: Update Card Components
1. **NodeCard.tsx**: Text colors, border colors, status badge variants, divider
2. **NodeDetail.tsx**: Back button, heading, status panel, statistics card, sensor data card, error/loading states, all text color classes
3. **NodeNameEditor.tsx**: View mode and edit mode text/label colors
4. **Settings.tsx**: Text colors, test result banners, about card

### Step 5: Update Form & Utility Components
1. **TimeRangeSelector.tsx**: Button inactive state, datetime input styles
2. **CreateZoneModal.tsx**: Modal panel bg, border, form elements, color picker border
3. **BatteryGauge.tsx**: Label text color
4. **SignalStrength.tsx**: Label text color
5. **HealthStatus.tsx**: Label text color, tooltip already uses dark bg (verify contrast)

### Step 6: Update Charts
1. **SensorChart.tsx**: Consume theme context, conditionally set gridcolor, tick font colors, axis title colors
2. **index.css**: Add `.dark .hoverlayer .hovertext path` override for dark tooltip bg

### Step 7: Testing & Polish
1. Verify all pages in both modes (Nodes list, Node detail, Settings)
2. Verify modal overlay and panel contrast
3. Test theme toggle persistence (refresh page, reopen browser)
4. Test OS preference detection (no stored value)
5. Verify chart readability in dark mode
6. Check focus ring visibility in dark mode (`ring-bramble-500` should be fine)
7. Verify zone color indicators are visible against dark backgrounds

## Risks & Considerations

- **Plotly integration**: Plotly uses its own styling system, not Tailwind. Must pass theme-aware colors via props. The hover tooltip needs CSS overrides. A re-render will be needed on theme change.
- **Zone colors**: User-chosen hex colors for zones may have poor contrast on dark backgrounds. Consider adding a subtle border or text shadow if needed. Monitor but don't over-engineer initially.
- **Selected color picker border**: In `CreateZoneModal`, the selected color swatch uses `border-gray-800` which won't be visible on dark bg. Needs `dark:border-gray-200`.
- **Transition animation**: Consider adding `transition-colors duration-200` to body/root for smooth toggling. Keep it optional - don't add if it causes flash of wrong theme on load.

## Non-Goals (Out of Scope)

- Custom theme colors beyond light/dark (e.g., high contrast mode)
- Per-user theme sync across devices (only localStorage)
- Dark mode for the Flask API or any non-dashboard UI
- Automatic theme scheduling (e.g., dark at night)
