/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        bramble: {
          50: '#f0fdf4',
          100: '#dcfce7',
          200: '#bbf7d0',
          300: '#86efac',
          400: '#4ade80',
          500: '#22c55e',
          600: '#16a34a',
          700: '#15803d',
          800: '#166534',
          900: '#14532d',
        },
        brand: {
          DEFAULT: '#4a7c59',
          light: '#5a9c6d',
          dark: '#3d6849',
        },
        status: {
          online: '#22c55e',
          offline: '#9ca3af',
          error: '#ef4444',
          warning: '#f59e0b',
          degraded: '#f97316',
        },
        surface: {
          DEFAULT: '#f8f8f9',
          card: '#ffffff',
          muted: '#f3f4f6',
        },
      }
    },
  },
  plugins: [],
}
