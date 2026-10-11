/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        ink: '#151B27',      // app background: deep blue slate
        slab: '#1C2433',     // panels
        well: '#121722',     // inset areas (editor, grids)
        rule: '#2A3448',     // borders
        chalk: '#DDE4EF',    // primary text
        haze: '#8D9AB0',     // secondary text
        // Operator families — used consistently in every view.
        scan: '#E9B44C',
        join: '#5FB8E8',
        reshape: '#B49BEA',
        flow: '#63CDB0',
        coral: '#EE7D6A',
      },
      fontFamily: {
        sans: ['"IBM Plex Sans"', 'system-ui', 'sans-serif'],
        mono: ['"JetBrains Mono"', 'ui-monospace', 'monospace'],
      },
      fontSize: {
        xs: ['0.75rem', '1.1rem'],
        sm: ['0.8125rem', '1.25rem'],
        base: ['0.875rem', '1.4rem'],
        lg: ['1.0625rem', '1.55rem'],
        xl: ['1.375rem', '1.8rem'],
      },
    },
  },
  plugins: [],
};
