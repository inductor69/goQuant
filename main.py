import asyncio
import json
import websockets
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
import ccxt.async_support as ccxt
from pydantic import BaseModel
from fastapi.middleware.cors import CORSMiddleware
import logging

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI()
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Serve static files
app.mount("/static", StaticFiles(directory="static"), name="static")

class DeribitWS:
    def __init__(self):
        self.ws = None
        self.orderbook_cache = {}
        self.clients = {}  # This should already be a dictionary
        self.request_id = 0
        self.response_futures = {}
        self.message_queue = asyncio.Queue()
        self.access_token = None
        self.refresh_token = None
        self.ids = {
            "authenticate": 2,
        }
        self.json = {
            "jsonrpc": "2.0",
            "id": None,
            "method": None,
            "params": None
        }
        self.positions_cache = {}
        self.available_markets = []  # Add this line to store available markets
        self.available_instruments = {
            'spot': [],
            'futures': [],
            'options': []
        }

    async def connect(self, url, client_id, client_secret):
        try:
            self.ws = await asyncio.wait_for(websockets.connect(url), timeout=10)
            self.client_id = client_id
            self.client_secret = client_secret
            asyncio.create_task(self.message_handler())
            await self.authenticate()
        except asyncio.TimeoutError:
            logger.error("Timeout while connecting to WebSocket")
            raise
        except Exception as e:
            logger.error(f"Error connecting to WebSocket: {str(e)}")
            raise

    async def authenticate(self):
        try:
            options = {
                "grant_type": "client_credentials",
                "client_id": self.client_id,
                "client_secret": self.client_secret
            }

            self.json["method"] = "public/auth"
            self.json["id"] = self.ids["authenticate"]
            self.json["params"] = options
            auth = json.dumps(self.json)
            
            await self.ws.send(auth)
            response = await self.message_queue.get()
            response_data = json.loads(response)
            
            if 'result' in response_data:
                self.access_token = response_data['result']['access_token']
                self.refresh_token = response_data['result']['refresh_token']
                logger.info("Authentication successful")
            else:
                logger.error(f"Authentication failed: {response_data}")
                raise Exception("Authentication failed")
        except Exception as e:
            logger.error(f"Error during authentication: {str(e)}")
            raise

    async def message_handler(self):
        while True:
            try:
                message = await self.ws.recv()
                await self.message_queue.put(message)
            except websockets.exceptions.ConnectionClosed:
                logger.error("WebSocket connection closed")
                break
            except Exception as e:
                logger.error(f"Error in message_handler: {str(e)}")

    async def process_messages(self):
        while True:
            message = await self.message_queue.get()
            data = json.loads(message)
            if 'id' in data:
                if data['id'] in self.response_futures:
                    self.response_futures[data['id']].set_result(data)
            elif 'method' in data and data['method'] == 'subscription':
                if 'channel' in data['params']:
                    if data['params']['channel'].startswith('book.'):
                        await self.process_orderbook(data['params'])
                    elif data['params']['channel'] == 'user.portfolio.btc':
                        await self.process_positions(data['params'])

    async def process_orderbook(self, data):
        instrument = data['data']['instrument_name']
        self.orderbook_cache[instrument] = {
            'bids': data['data']['bids'],
            'asks': data['data']['asks'],
            'timestamp': data['data']['timestamp']
        }
        await self.broadcast(instrument)

    async def process_positions(self, data):
        self.positions_cache = data['data']
        await self.broadcast_positions()

    async def broadcast_positions(self):
        message = json.dumps({"type": "positions", "data": self.positions_cache})
        for client in self.clients:
            try:
                await client.send_text(message)
            except WebSocketDisconnect:
                await self.remove_client(client)

    async def broadcast(self, instrument):
        message = json.dumps({"type": "orderbook", "instrument": instrument, "data": self.orderbook_cache[instrument]})
        for client, subscriptions in self.clients.items():
            if instrument in subscriptions:
                try:
                    await client.send_text(message)
                except WebSocketDisconnect:
                    await self.remove_client(client)

    async def send_request(self, method, params):
        self.request_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": self.request_id,
            "method": method,
            "params": params
        }
        future = asyncio.Future()
        self.response_futures[self.request_id] = future
        await self.ws.send(json.dumps(request))
        return await future

    async def buy_raw(self, instrument_name, amount, order_type, reduce_only, price, post_only):
        params = {
            "instrument_name": instrument_name,
            "amount": amount,
            "type": order_type,
            "reduce_only": reduce_only,
            "post_only": post_only
        }
        if price is not None:
            params["price"] = price
        return await self.send_request("private/buy", params)

    async def sell_raw(self, instrument_name, amount, order_type, reduce_only, price, post_only):
        params = {
            "instrument_name": instrument_name,
            "amount": amount,
            "type": order_type,
            "reduce_only": reduce_only,
            "post_only": post_only
        }
        if price is not None:
            params["price"] = price
        return await self.send_request("private/sell", params)

    async def add_client(self, websocket: WebSocket):
        await websocket.accept()
        self.clients[websocket] = set()

    async def remove_client(self, websocket: WebSocket):
        if websocket in self.clients:
            for instrument in self.clients[websocket]:
                await self.unsubscribe_orderbook(instrument)
            del self.clients[websocket]

    async def subscribe_client(self, websocket: WebSocket, instrument: str):
        if websocket in self.clients:
            self.clients[websocket].add(instrument)
            await self.subscribe_orderbook(instrument)

    async def unsubscribe_client(self, websocket: WebSocket, instrument: str):
        if websocket in self.clients and instrument in self.clients[websocket]:
            self.clients[websocket].remove(instrument)
            await self.unsubscribe_orderbook(instrument)

    async def subscribe_orderbook(self, instrument):
        if instrument not in self.orderbook_cache:
            await self.send_request("public/subscribe", {
                "channels": [f"book.{instrument}.none.10.100ms"]
            })
            self.orderbook_cache[instrument] = {}

    async def unsubscribe_orderbook(self, instrument):
        if instrument in self.orderbook_cache and not any(instrument in subs for subs in self.clients.values()):
            await self.send_request("public/unsubscribe", {
                "channels": [f"book.{instrument}.none.10.100ms"]
            })
            del self.orderbook_cache[instrument]

    async def subscribe_positions(self):
        await self.send_request("private/subscribe", {
            "channels": ["user.portfolio.btc"]
        })

    async def get_available_markets(self):
        response = await self.send_request("public/get_instruments", {
            "currency": "BTC",
            "kind": "future"
        })
        self.available_markets = [
            {
                'instrument_name': instrument['instrument_name'],
                'contract_size': instrument['contract_size']
            }
            for instrument in response['result']
        ]
        return self.available_markets

    async def handle_websocket(self, websocket: WebSocket):
        await websocket.accept()
        self.clients[websocket] = set()
        try:
            while True:
                data = await websocket.receive_text()
                message = json.loads(data)
                if message['action'] == 'get_available_markets':
                    markets = await self.get_available_markets()
                    await websocket.send_json({"type": "available_markets", "data": markets})
                elif message['action'] == 'subscribe':
                    instrument = message['instrument']
                    self.clients[websocket].add(instrument)
                    await self.subscribe_orderbook(instrument)
                elif message['action'] == 'unsubscribe':
                    instrument = message['instrument']
                    self.clients[websocket].remove(instrument)
                    await self.unsubscribe_orderbook(instrument)
                elif message['action'] == 'place_order':
                    response = await self.place_order(message)
                    await websocket.send_json(response)
                elif message['action'] == 'cancel_order':
                    response = await self.cancel_order(message)
                    await websocket.send_json(response)
                elif message['action'] == 'modify_order':
                    response = await self.modify_order(message)
                    await websocket.send_json(response)
                elif message['action'] == 'get_positions':
                    positions = await self.get_positions()
                    await websocket.send_json(positions)
                elif message['action'] == 'get_open_orders':
                    open_orders = await self.get_open_orders()
                    await websocket.send_json(open_orders)
                elif message['action'] == 'get_available_instruments':
                    instruments = await self.get_available_instruments()
                    await websocket.send_json({"type": "available_instruments", "data": instruments})
                # ... handle other actions ...
        except WebSocketDisconnect:
            await self.remove_client(websocket)
        except Exception as e:
            logging.error(f"Error in handle_websocket: {str(e)}")
            await websocket.send_json({"type": "error", "data": {"message": str(e)}})

    async def place_order(self, order_data):
        try:
            # Extract order details
            symbol = order_data['symbol']
            order_type = order_data['type']
            side = order_data['side']
            amount = order_data['amount']
            price = order_data.get('price')  # Price is optional for market orders

            # Validate order data
            if not all([symbol, order_type, side, amount]):
                raise ValueError("Missing required order parameters")

            # Place the order using the appropriate Deribit API method
            if side == 'buy':
                response = await self.buy_raw(symbol, amount, order_type, False, price, False)
            elif side == 'sell':
                response = await self.sell_raw(symbol, amount, order_type, False, price, False)
            else:
                raise ValueError("Invalid order side")

            # Check if the order was placed successfully
            if 'result' in response and 'order' in response['result']:
                return {
                    "type": "order_response",
                    "data": {
                        "success": True,
                        "order": response['result']['order']
                    }
                }
            elif 'error' in response:
                error_message = response['error'].get('message', 'Unknown error')
                error_data = response['error'].get('data', {})
                return {
                    "type": "order_response",
                    "data": {
                        "success": False,
                        "error": f"{error_message}: {error_data}"
                    }
                }
            else:
                raise ValueError(f"Unexpected response from Deribit API: {response}")

        except Exception as e:
            logging.error(f"Error placing order: {str(e)}")
            return {
                "type": "order_response",
                "data": {
                    "success": False,
                    "error": str(e)
                }
            }

    async def get_positions(self):
        try:
            response = await self.send_request("private/get_positions", {"currency": "BTC"})
            if 'result' in response:
                return {
                    "type": "positions",
                    "data": response['result']
                }
            else:
                raise ValueError(f"Unexpected response from Deribit API: {response}")
        except Exception as e:
            logging.error(f"Error fetching positions: {str(e)}")
            return {
                "type": "positions",
                "data": []
            }

    async def get_open_orders(self):
        try:
            response = await self.send_request("private/get_open_orders_by_currency", {"currency": "BTC"})
            if 'result' in response:
                return {
                    "type": "open_orders",
                    "data": response['result']
                }
            else:
                raise ValueError(f"Unexpected response from Deribit API: {response}")
        except Exception as e:
            logging.error(f"Error fetching open orders: {str(e)}")
            return {
                "type": "open_orders",
                "data": []
            }

    async def cancel_order(self, order_data):
        try:
            order_id = order_data['order_id']
            response = await self.send_request("private/cancel", {"order_id": order_id})

            if 'result' in response:
                return {
                    "type": "order_response",
                    "data": {
                        "success": True,
                        "order": response['result']
                    }
                }
            elif 'error' in response:
                return {
                    "type": "order_response",
                    "data": {
                        "success": False,
                        "error": response['error']['message']
                    }
                }
            else:
                raise ValueError(f"Unexpected response from Deribit API: {response}")

        except Exception as e:
            logging.error(f"Error cancelling order: {str(e)}")
            return {
                "type": "order_response",
                "data": {
                    "success": False,
                    "error": str(e)
                }
            }

    async def modify_order(self, order_data):
        try:
            order_id = order_data['order_id']
            params = {"order_id": order_id}
            
            if 'amount' in order_data:
                params['amount'] = order_data['amount']
            if 'price' in order_data:
                params['price'] = order_data['price']

            response = await self.send_request("private/edit", params)

            if 'result' in response:
                return {
                    "type": "order_response",
                    "data": {
                        "success": True,
                        "order": response['result']
                    }
                }
            elif 'error' in response:
                return {
                    "type": "order_response",
                    "data": {
                        "success": False,
                        "error": response['error']['message']
                    }
                }
            else:
                raise ValueError(f"Unexpected response from Deribit API: {response}")

        except Exception as e:
            logging.error(f"Error modifying order: {str(e)}")
            return {
                "type": "order_response",
                "data": {
                    "success": False,
                    "error": str(e)
                }
            }

    async def get_available_instruments(self):
        currencies = ['BTC', 'ETH']  # Add more currencies if needed
        instrument_types = ['spot', 'future', 'option']
        
        for currency in currencies:
            for instrument_type in instrument_types:
                response = await self.send_request("public/get_instruments", {
                    "currency": currency,
                    "kind": instrument_type
                })
                if 'result' in response:
                    category = instrument_type + 's' if instrument_type != 'spot' else 'spot'
                    self.available_instruments[category].extend([
                        {
                            'instrument_name': instrument['instrument_name'],
                            'kind': instrument['kind'],
                            'contract_size': instrument.get('contract_size', 1),
                            'settlement_period': instrument.get('settlement_period', 'perpetual'),
                            'quote_currency': instrument['quote_currency'],
                            'base_currency': instrument['base_currency']
                        }
                        for instrument in response['result']
                    ])
        
        return self.available_instruments

deribit_ws = DeribitWS()

@app.on_event("startup")
async def startup_event():
    client_id = "o9-Ru8mA"  # Replace with your actual client ID
    client_secret = "Saia2HHqtUKjOKjHyo0Nodj7z-U78Bvp9pKG_vxtOV8"  # Replace with your actual client secret
    try:
        await deribit_ws.connect('wss://test.deribit.com/ws/api/v2', client_id, client_secret)
        asyncio.create_task(deribit_ws.process_messages())
        await deribit_ws.subscribe_positions()
        logger.info("Startup completed successfully")
    except Exception as e:
        logger.error(f"Error during startup: {str(e)}")
        # You might want to exit the application here if startup failsx
        # import sys
        # sys.exit(1)

@app.get("/")
async def read_root():
    return FileResponse("static/index.html")

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await deribit_ws.handle_websocket(websocket)

class OrderRequest(BaseModel):
    symbol: str
    type: str
    side: str
    amount: float
    price: float = None

@app.post("/place_order")
async def place_order(order: OrderRequest):
    try:
        if order.side == "buy":
            response = await deribit_ws.buy_raw(
                order.symbol,
                order.amount,
                order.type,
                False,  # reduce_only
                order.price,
                False  # post_only
            )
        elif order.side == "sell":
            response = await deribit_ws.sell_raw(
                order.symbol,
                order.amount,
                order.type,
                False,  # reduce_only
                order.price,
                False  # post_only
            )
        else:
            raise HTTPException(status_code=400, detail="Invalid order side")
        
        return response  # Return the full API response
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Error placing order: {str(e)}")

@app.post("/edit_order")
async def edit_order(order_id: str, amount: float, price: float):
    response = await deribit_ws.edit(order_id, amount, price)
    return json.loads(response)

@app.post("/cancel_order")
async def cancel_order(order_id: str):
    response = await deribit_ws.cancel(order_id)
    return json.loads(response)

@app.post("/cancel_all_orders")
async def cancel_all_orders():
    response = await deribit_ws.cancel_all()
    return json.loads(response)

@app.get("/account_summary/{currency}")
async def get_account_summary(currency: str):
    response = await deribit_ws.account_summary(currency)
    return json.loads(response)

@app.get("/ticker/{instrument_name}")
async def get_ticker(instrument_name: str):
    response = await deribit_ws.ticker(instrument_name)
    return json.loads(response)

@app.get("/get_positions")
async def get_positions():
    try:
        response = await deribit_ws.send_request("private/get_positions", {"currency": "BTC"})
        return response
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Error fetching positions: {str(e)}")

@app.on_event("shutdown")
async def shutdown_event():
    await deribit_ws.close()

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000, log_level="info")