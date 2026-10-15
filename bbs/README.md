# MarketSim

A modular quantitative trading strategy simulator for developing, backtesting, and comparing systematic trading strategies. Built with a pluggable architecture supporting 19 signal generators, 4 forecasting models, 7 position sizers, and full optimization/walk-forward validation.

## Quick Start

```bash
# Clone and install
cd MarketSim
python -m venv .venv && source .venv/bin/activate
pip install -e .

# Run a backtest with default settings (synthetic data)
marketsim run

# Compare multiple strategy variants
marketsim compare

# Launch interactive REPL
marketsim interactive

# Launch GUI (requires PyQt6)
pip install -e ".[gui]"
marketsim gui
```

## Installation

**Core (required):**
```bash
pip install -e .
```

**Optional extras:**
```bash
pip install -e ".[ml]"         # XGBoost + LightGBM models
pip install -e ".[gui]"        # PyQt6 graphical interface
pip install -e ".[cli-extra]"  # Tab completion + terminal plots in REPL
pip install -e ".[pdf]"        # PDF report generation (weasyprint)
pip install -e ".[tda]"        # Topological data analysis (giotto-tda, ripser)
pip install -e ".[dev]"        # pytest, ruff, mypy
pip install -e ".[all]"        # Everything
```

**API data providers (install separately):**
```bash
pip install yfinance           # Yahoo Finance
pip install alpha_vantage      # Alpha Vantage (requires API key)
pip install polygon-api-client # Polygon.io (requires API key)
```

Requires Python 3.11+.

---

## Data Input

MarketSim supports four categories of data input. Synthetic data is the default and requires no external files or API keys.

### 1. Synthetic Data (Built-in)

Generate artificial market data with configurable statistical properties. Five generators are available:

| Generator | Description | Use Case |
|---|---|---|
| `gbm` | Geometric Brownian Motion | Baseline lognormal price process |
| `ou` | Ornstein-Uhlenbeck | Mean-reverting asset prices |
| `regime_switching` | Markov regime model | Bull/bear/sideways transitions with state-dependent drift and volatility |
| `correlated` | Cholesky-correlated GBM | Multi-asset portfolios with realistic cross-correlation |
| `merton` | Merton jump-diffusion | GBM + Poisson crash events for fat-tailed returns |

Synthetic data can embed known patterns for strategy validation:
- **Cycles** — sinusoidal components with configurable period and amplitude
- **Seasonality** — day-of-week and month-of-year effects
- **Anomalies** — random gaps and spikes at configurable frequencies
- **Lead-lag** — cross-asset dependencies where one asset leads another
- **Volume dynamics** — realistic volume with informed trading episodes

```yaml
# config.yaml
data:
  provider: "synthetic"
  synthetic:
    num_assets: 20
    num_bars: 5000
    generator: "regime_switching"
    drift: 0.05
    volatility: 0.20
    correlation: 0.3
    embedded_patterns:
      cycles:
        - { period: 20, amplitude: 0.02 }
        - { period: 60, amplitude: 0.03 }
      anomalies:
        gap_frequency: 0.01
        spike_frequency: 0.005
```

### 2. Historical Files (CSV / Parquet)

Load your own market data from CSV or Parquet files.

**Expected directory structure:**
```
data/historical/
  AAPL.csv
  MSFT.csv
  GOOG.parquet
```

**Expected CSV format:**
```csv
date,open,high,low,close,volume
2023-01-03,130.28,131.04,129.89,130.21,112117500
2023-01-04,131.25,133.51,130.06,126.36,89113600
```

The symbol is inferred from the filename (e.g., `AAPL.csv` becomes symbol `AAPL`). If a `symbol` column exists in the file, it is used instead.

```yaml
data:
  provider: "historical"
  historical:
    path: "data/historical/"
    symbols: ["AAPL", "MSFT"]  # Optional: filter to specific symbols
```

You can also point to a single file:
```yaml
data:
  provider: "historical"
  historical:
    path: "data/historical/AAPL.csv"
```

The loader auto-detects file type by extension (`.csv`, `.tsv`, `.parquet`) and handles forward-filling of missing values.

### 3. Yahoo Finance API

Fetch historical OHLCV data from Yahoo Finance. Requires `pip install yfinance`.

```yaml
data:
  provider: "yahoo"
  historical:
    symbols: ["AAPL", "MSFT", "GOOG", "AMZN"]
simulation:
  start_date: "2020-01-01"
  end_date: "2025-01-01"
```

Supports daily, hourly, and 1-minute frequencies. No API key required.

### 4. Alpha Vantage API

Fetch data from Alpha Vantage. Requires `pip install alpha_vantage` and an API key.

```bash
export ALPHA_VANTAGE_API_KEY="your_key_here"
```

```yaml
data:
  provider: "alpha_vantage"
  historical:
    symbols: ["AAPL", "MSFT"]
```

### 5. Polygon.io API

Fetch data from Polygon.io. Requires `pip install polygon-api-client` and an API key.

```bash
export POLYGON_API_KEY="your_key_here"
```

```yaml
data:
  provider: "polygon"
  historical:
    symbols: ["AAPL", "MSFT"]
simulation:
  start_date: "2020-01-01"
  end_date: "2025-01-01"
```

### Data Validation

All data (synthetic or real) passes through automatic validation before entering the backtest pipeline:

- Missing values are forward-filled then back-filled
- Negative/zero prices are clipped to a minimum floor
- Extreme single-bar jumps (>50%) are flagged as warnings
- Duplicate timestamps are deduplicated
- Stale/flat series are detected and warned about
- OHLCV bar consistency is verified (High >= Open/Close, Low <= Open/Close)

---

## What the Application Does

### Backtest Pipeline

MarketSim runs a complete quantitative backtesting pipeline:

```
Data (prices + volumes)
  |
  v
Signal Generation (19 generators compute alpha signals)
  |
  v
Forecast Model (combine signals into return predictions)
  |
  v
Position Sizing (convert forecasts to target positions)
  |
  v
Risk Management (enforce leverage, concentration, drawdown limits)
  |
  v
Trade Execution (apply transaction costs: commission, spread, slippage, market impact)
  |
  v
Performance Metrics (Sharpe, Sortino, Calmar, drawdown, win rate, etc.)
```

Each bar is processed sequentially. Signals are computed on the full price history up to that bar, positions are sized and risk-checked, and trades are executed with realistic cost modeling.

### Signal Generators (19)

**Core signals (8):**

| Signal | Technique | What It Detects |
|---|---|---|
| `mean_reversion` | Z-score of price vs moving average | Overbought/oversold conditions |
| `momentum` | Moving average crossover + rate of change | Trend direction and strength |
| `fft_spectral` | Fast Fourier Transform | Cyclical patterns in price data |
| `hmm_regime` | Hidden Markov Model (Baum-Welch) | Market regime (bull/bear/sideways) |
| `statarb` | Cointegration + spread z-score | Pairs trading opportunities |
| `anomaly` | Volume spikes, gaps, volatility shifts | Unusual market events |
| `seasonal` | Day-of-week, month-of-year effects | Calendar anomalies |
| `kernel_features` | Kernel regression in feature space | Nonlinear price patterns |

**Novel/experimental signals (9):**

| Signal | Technique | What It Detects |
|---|---|---|
| `wavelet_fft` | Wavelet decomposition + FFT | Multi-scale frequency patterns |
| `topo_regime` | Topological data analysis (persistent homology proxies) | Regime transitions before they complete |
| `causality_net` | Transfer entropy + Granger causality | Causal information flow between assets |
| `spectral_pairs` | Cross-spectral coherence | Frequency-domain pair relationships |
| `resonance` | Cross-frequency energy coupling | Harmonic amplification patterns |
| `fractal_switch` | Fractal dimension (Hurst/DFA) | Mean-reverting vs trending regime |
| `flow_toxicity` | VPIN-inspired order flow analysis | Informed trading / adverse selection |
| `stochastic_res` | Stochastic resonance exploitation | Noise-enhanced weak signals |
| `hurst` | Hurst exponent estimation | Long-memory vs anti-persistence |

### Forecasting Models (4)

| Model | Method |
|---|---|
| `ensemble` | Weighted signal combination (equal-weight, inverse-volatility, ridge regression, performance-based) |
| `sde` | Stochastic differential equation (OU / GBM parameter fitting) |
| `ml_gbm` | Gradient boosted trees (XGBoost / LightGBM / sklearn) |
| `topo_fusion` | Topological stability-weighted signal fusion |

### Position Sizers (7)

| Sizer | Method |
|---|---|
| `equal_weight` | Equal allocation across active positions |
| `kelly` | Full/fractional Kelly criterion with correlation adjustment |
| `volatility_target` | Target a specific portfolio volatility level |
| `entropy_kelly` | Entropy-gated adaptive Kelly (reduces sizing in uncertain regimes) |
| `mean_variance` | Markowitz mean-variance optimization |
| `risk_parity` | Risk parity (equal risk contribution) |
| `max_diversification` | Maximum diversification ratio |

### Risk Management

- Maximum position concentration per asset
- Portfolio leverage limits (default 5x, configurable)
- Maximum drawdown stop-loss
- Volatility scaling (reduce exposure in high-vol regimes)

### Transaction Costs

Four cost model levels:
- `zero` — no costs (for signal evaluation)
- `fixed` — flat commission per trade
- `realistic` — commission (bps) + bid-ask spread (bps) + slippage (bps)
- `sqrt` impact — adds square-root market impact model on top of realistic costs

### Performance Metrics (18)

Every backtest produces: `total_return`, `annual_return`, `annual_volatility`, `sharpe`, `sortino`, `calmar`, `information_ratio`, `max_drawdown`, `max_drawdown_duration`, `var_95`, `cvar_95`, `total_trades`, `win_rate`, `avg_win`, `avg_loss`, `profit_factor`, `skewness`, `kurtosis`.

---

## Interfaces

### CLI

```bash
marketsim run                              # Run single backtest
marketsim run --output-format json         # JSON output
marketsim compare                          # Compare strategy variants
marketsim optimize --method bayesian       # Parameter optimization
marketsim walk-forward --windows 5         # Walk-forward validation
marketsim list signals                     # List all signal generators
marketsim list models                      # List all models
marketsim describe signal fft_spectral     # Show signal parameters
marketsim data                             # Show data summary
marketsim report --format html             # Generate report
marketsim gui                              # Launch GUI
marketsim interactive                      # Launch REPL

# Configuration
marketsim --config my_config.yaml run
marketsim --set data.synthetic.num_bars=1000 run
marketsim --seed 123 compare
```

### Interactive REPL

```
$ marketsim interactive
MarketSim> load config/examples/mean_reversion.yaml
MarketSim> set strategy.signals.0.params.lookback 100
MarketSim> run
MarketSim> metrics
MarketSim> plot equity
MarketSim> compare
MarketSim> optimize
MarketSim> export html
MarketSim> help
MarketSim> quit
```

Features tab completion and command history when `prompt-toolkit` is installed.

### GUI (PyQt6)

Launch with `marketsim gui`. Provides 8 panels:

| Panel | Function |
|---|---|
| **Dashboard** | Overview with metric cards and equity curve |
| **Strategy Builder** | Visual strategy composition — add signals, select model/sizer, set risk limits |
| **Data** | Configure data source, preview data statistics |
| **Backtest** | Run/stop with live progress, real-time trade ticker |
| **Results** | Metrics table, equity/drawdown/rolling-Sharpe charts, trade log |
| **Optimization** | Grid/random/Bayesian parameter search with walk-forward settings |
| **Comparison** | Side-by-side metrics, overlaid equity curves, bootstrap significance tests |
| **Signal Inspector** | Signal timeline, distribution histogram, signal-vs-return scatter, FFT spectrum |

---

## Optimization & Evaluation

### Parameter Optimization

Three search methods:
- **Grid search** — exhaustive enumeration of parameter combinations
- **Random search** — N random samples from parameter ranges
- **Bayesian search** — Optuna TPE sampler for intelligent search

### Walk-Forward Validation

Rolling train/test windows to detect overfitting:
- Train parameters on in-sample window
- Test on out-of-sample window
- Step forward and repeat
- Reports efficiency ratio (OOS Sharpe / IS Sharpe) to flag overfitting

### Monte Carlo Robustness

Stress test strategies under perturbation:
- **Bootstrap** — resample returns with replacement
- **Noise** — inject Gaussian noise into returns
- **Regime shift** — randomly change volatility/drift parameters
- **Correlation** — perturb the correlation structure

### Benchmark Strategies

Four benchmarks for baseline comparison:

| Benchmark | Description |
|---|---|
| `buy_and_hold` | Constant equal-weight long exposure |
| `random_entry` | Random signals (null hypothesis) |
| `perfect_foresight` | Oracle with future return knowledge (upper bound) |
| `inverse_strategy` | Negated strategy signals (sanity check) |

### Reports

Generate self-contained reports in HTML, JSON, or PDF format containing metrics, charts, and trade statistics.

---

## Plugin System

Add custom signals, models, or sizers by placing Python files in the `plugins/` directory.

```python
# plugins/my_signal.py
from src.core.registry import register_signal
from src.signals.base import BaseSignal

@register_signal("my_signal")
class MySignal(BaseSignal):
    class Params:
        period: int = 20

    def compute(self, prices, volumes=None, **kwargs):
        # Return DataFrame with same shape as prices, values in [-1, 1]
        return prices.pct_change(self.params["period"]).clip(-1, 1)
```

Plugins are auto-discovered and registered. They immediately appear in `marketsim list signals` and can be used in config files and the GUI strategy builder.

See `plugins/example_signal.py` for a complete example.

---

## Configuration

Configuration uses YAML files. See `config/default.yaml` for the full schema with all defaults.

Example configuration files are provided in `config/examples/`:
- `mean_reversion.yaml` — Simple mean reversion strategy
- `statarb_pairs.yaml` — Statistical arbitrage pairs trading
- `multi_strategy.yaml` — Multi-signal combined strategy
- `novel_wavelet_fft.yaml` — Novel wavelet-FFT strategy

Key configuration sections:

```yaml
simulation:
  name: "my_backtest"
  seed: 42
  initial_capital: 1000000

data:
  provider: "synthetic"        # synthetic | historical | yahoo | alpha_vantage | polygon
  synthetic:
    num_assets: 20
    num_bars: 5000
    generator: "regime_switching"

strategy:
  name: "My Strategy"
  signals:
    - name: "mean_reversion"
      params: { lookback: 60, entry_z: 2.0 }
      weight: 1.0
    - name: "momentum"
      params: { fast_period: 10, slow_period: 30 }
      weight: 0.8
  model:
    name: "ensemble"
    params: { method: "ridge" }
  position_sizer:
    name: "kelly"
    params: { fraction: 0.25 }
  risk:
    max_position_pct: 0.10
    max_leverage: 5.0
    max_drawdown_stop: 0.15

execution:
  cost_model: "realistic"
  commission_bps: 1.0
  spread_bps: 2.0
  slippage_bps: 0.5
```

Use `--set` flags to override any value from the command line:
```bash
marketsim --set data.synthetic.num_bars=1000 --set strategy.signals.0.params.lookback=100 run
```

---

## Project Structure

```
MarketSim/
  config/
    default.yaml              # Default configuration with all options
    examples/                 # Example strategy configurations
  plugins/
    example_signal.py         # Example plugin (RSI signal)
  src/
    cli/
      commands.py             # CLI command definitions (click)
      interactive.py          # Interactive REPL mode
      runner.py               # Strategy execution orchestration
      formatters.py           # Console output formatting (rich)
    core/
      types.py                # Data types (Trade, Position, BacktestResult, etc.)
      registry.py             # Component registry (signals, models, sizers)
      config.py               # Configuration loading and validation
      events.py               # Event bus for engine communication
      cache.py                # Disk-backed computation cache
      logging.py              # Structured logging
      parallel.py             # Parallel execution utilities
    data/
      synthetic.py            # 5 synthetic generators (GBM, OU, regime, correlated, Merton)
      historical.py           # CSV/Parquet loader + Yahoo/AlphaVantage/Polygon providers
      validation.py           # Price and OHLCV data validation
      features.py             # Feature engineering pipeline
      base.py                 # Abstract data provider interface
    execution/
      engine.py               # Backtest execution engine
      costs.py                # Transaction cost models
    signals/                  # 17 signal generators (8 core + 9 novel)
    models/                   # 4 forecasting models (ensemble, SDE, ML, topo fusion)
    portfolio/                # 7 position sizers + risk management
    optimization/             # Parameter search + walk-forward validation
    evaluation/               # Metrics, comparison, Monte Carlo, reports, benchmarks
    strategies/               # Adaptive strategy selection
    plugins/                  # Plugin auto-discovery loader
    gui/                      # PyQt6 GUI (8 panels + chart widgets + styles)
  tests/                      # 189 tests
```

---

## Testing

```bash
# Run all tests
.venv/bin/pytest

# Run with verbose output
.venv/bin/pytest -v

# Run specific test file
.venv/bin/pytest tests/test_signals.py

# Run specific test class
.venv/bin/pytest tests/test_novel_strategies.py::TestWaveletFFT
```

Current test suite: **189 tests passing** across 11 test files covering all components.

---

## Requirements

- Python 3.11+
- Core: numpy, scipy, pandas, scikit-learn, statsmodels, hmmlearn, PyWavelets, optuna, click, rich, jinja2, pyyaml, networkx
- Optional: PyQt6, pyqtgraph (GUI), xgboost/lightgbm (ML models), weasyprint (PDF), prompt-toolkit (REPL), yfinance/alpha_vantage/polygon (API data)

See `pyproject.toml` for the complete dependency list.
