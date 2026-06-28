# limit-order-book

A limit order book with a matching engine, building up to a simple
market-making strategy on top of it.

## Design

An `OrderBook` holds resting orders; a `MatchingEngine` decides the *rule* by
which an incoming order matches against it. Every market type is just a
different `MatchingEngine`, so they all share one interface.

Two independent axes:

| axis    | options                                            |
|---------|----------------------------------------------------|
| timing  | continuous matching · call auction (batch & cross) |
| pricing | uniform (single price) · discriminatory (pay-as-bid) |

Pricing is a pluggable `PricingRule` strategy, so a single `CallAuction` class
supports both uniform and discriminatory clearing.

```
python_engine/
├── engine.py                  # MatchingEngine interface
├── orderbook/                 # OrderBook + Order/Trade/Side dataclasses
├── continuous_auction/        # ContinuousDoubleAuction
└── call_auction/              # CallAuction + pricing rules
```

## Requirements

- Python 3.12+
- `make`

## Setup

The `Makefile` manages a `.venv` virtual environment for you.

```sh
make venv      # create .venv and install dependencies
make run       # run the matching engine
make test      # run the test suite
make clean     # remove .venv and caches
make help      # list available targets
```

### Manual setup

If you prefer not to use `make`:

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python -m python_engine.engine
```
