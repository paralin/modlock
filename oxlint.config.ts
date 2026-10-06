import { defineConfig } from 'oxlint'

// Lint uses the oxlint correctness category with type-aware rules, which read
// the nearest tsconfig of each file.
export default defineConfig({
  plugins: ['typescript', 'unicorn'],
  categories: { correctness: 'error' },
  env: { builtin: true },
  options: { typeAware: true },
  ignorePatterns: [
    'build*/**',
    'third_party/**',
    'vendor/**',
    'node_modules/**',
    '.tools/**',
    '.tmp/**',
    '**/*.pb.ts',
    '**/*.pb.js',
    '**/*.gen.ts',
    'js/src/entities.ts',
  ],
  rules: {
    'no-unused-vars': [
      'error',
      {
        argsIgnorePattern: '^_',
        varsIgnorePattern: '^_',
        caughtErrors: 'none',
      },
    ],
    'typescript/ban-ts-comment': 'error',
    'typescript/no-explicit-any': 'error',
    'typescript/no-floating-promises': 'error',
    'typescript/no-misused-promises': 'error',
    'typescript/no-unnecessary-type-assertion': 'error',
    'typescript/no-unsafe-argument': 'error',
    'typescript/no-unsafe-assignment': 'error',
    'typescript/no-unsafe-call': 'error',
    'typescript/no-unsafe-member-access': 'error',
    'typescript/no-unsafe-return': 'error',
    'typescript/only-throw-error': 'error',
    'typescript/require-await': 'error',
    'typescript/restrict-plus-operands': 'error',
  },
})
