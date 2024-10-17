# Deribit API Client

This project provides a C++ client for interacting with the Deribit cryptocurrency derivatives exchange API. It includes both REST API and WebSocket functionality for real-time order book data.

## Table of Contents
- [Features](#features)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Usage](#usage)
- [File Structure](#file-structure)
- [Contributing](#contributing)
- [License](#license)

## Features

- REST API client for Deribit
  - Authentication
  - Place, modify, and cancel orders
  - Get positions
  - Fetch order book data
- WebSocket client for real-time order book updates
- Command-line interface for interacting with the API

## Prerequisites

- C++11 or later
- libcurl
- JsonCpp
- Boost (Beast, Asio)
- OpenSSL

## Installation

1. Clone the repository:
   ```
   git clone https://github.com/yourusername/deribit-api-client.git
   cd deribit-api-client
   ```

2. Install the required dependencies. On Ubuntu, you can use:
   ```
   sudo apt-get install libcurl4-openssl-dev libjsoncpp-dev libboost-all-dev libssl-dev
   ```

3. Compile the project:
   ```
   g++ -std=c++11 main.cpp -o deribit_client -lcurl -ljsoncpp
   g++ -std=c++11 websocket.cpp -o websocket_client -lboost_system -lssl -lcrypto
   ```

## Usage

### REST API Client

Run the compiled REST API client:

```
./main
```

Follow the on-screen menu to interact with the Deribit API.

### WebSocket Client

Run the compiled WebSocket client with an instrument name:

```
./websocket BTC-PERPETUAL
```

This will connect to the Deribit WebSocket API and stream real-time order book data for the specified instrument.

## File Structure

- `main.cpp`: Contains the REST API client implementation and command-line interface.
- `websocket.cpp`: Contains the WebSocket client implementation.

## Contributing

Contributions are welcome! Please see the [CONTRIBUTING.md](CONTRIBUTING.md) file for more information.

## License

This project is licensed under the [MIT License](LICENSE).

