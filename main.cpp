#include <iostream>
#include <cstdlib>
#include <curl/curl.h>
#include <json/json.h>
#include <string>
#include <chrono>
#include <thread>
#include <memory>

using namespace std;
using namespace std::chrono;

class DeribitAPI {
private:
    std::string access_token;
    steady_clock::time_point token_creation_time;
    int token_expiration_time = 900;
    const std::string BASE_URL = "https://test.deribit.com/api/v2/";
    const std::string CLIENT_ID = "o9-Ru8mA";
    const std::string API_KEY = "Saia2HHqtUKjOKjHyo0Nodj7z-U78Bvp9pKG_vxtOV8";

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    bool is_token_expired() const {
        steady_clock::time_point current_time = steady_clock::now();
        duration<double> elapsed_seconds = current_time - token_creation_time;
        return elapsed_seconds.count() >= token_expiration_time;
    }

    void refresh_token() {
        // Use class member variables instead of environment variables
        std::string token_url = BASE_URL + "public/auth?client_id=" + CLIENT_ID + 
                                "&client_secret=" + API_KEY + 
                                "&grant_type=client_credentials";

        std::string response = make_request("public/auth", 
            "client_id=" + CLIENT_ID + 
            "&client_secret=" + API_KEY + 
            "&grant_type=client_credentials", false);

        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream response_stream(response);

        if (Json::parseFromStream(reader, response_stream, &root, &errors)) {
            if (root["result"].isMember("access_token")) {
                access_token = root["result"]["access_token"].asString();
                token_creation_time = steady_clock::now();
                token_expiration_time = root["result"]["expires_in"].asInt();
                std::cout << "New token acquired, valid for " << token_expiration_time << " seconds." << std::endl;
            } else {
                std::cerr << "Failed to acquire token: " << root << std::endl;
            }
        } else {
            std::cerr << "Failed to parse the JSON response: " << errors << std::endl;
        }
    }

    std::string make_request(const std::string& endpoint, const std::string& params, bool auth) {
        std::string readBuffer;
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);

        if (curl) {
            std::string url = BASE_URL + endpoint + "?" + params;
            curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &readBuffer);

            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
            if (auth) {
                headers.reset(curl_slist_append(nullptr, ("Authorization: Bearer " + access_token).c_str()));
                curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
            }

            CURLcode res = curl_easy_perform(curl.get());
            if (res != CURLE_OK) {
                throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(res)));
            }
        } else {
            throw std::runtime_error("Failed to initialize CURL");
        }

        return readBuffer;
    }

public:
    DeribitAPI() = default;

    std::string get_token() {
        if (access_token.empty() || is_token_expired()) {
            refresh_token();
        }
        return access_token;
    }

    void place_order() {
        try {
            if (get_token().empty()) {
                throw std::runtime_error("Authentication failed");
            }

            std::string direction, instrument_name, type, label;
            float amount;

            std::cout << "Enter direction (buy/sell): ";
            std::cin >> direction;
            std::cout << "Enter instrument name (e.g., ETH-PERPETUAL): ";
            std::cin >> instrument_name;
            std::cout << "Enter amount: ";
            std::cin >> amount;
            std::cout << "Enter order type (e.g., market, limit): ";
            std::cin >> type;
            std::cout << "Enter label (optional identifier for the order): ";
            std::cin >> label;

            std::string params = "amount=" + std::to_string(amount) +
                                 "&instrument_name=" + instrument_name +
                                 "&label=" + label +
                                 "&type=" + type;

            std::string response = make_request("private/" + direction, params, true);

            Json::Value root;
            Json::CharReaderBuilder reader;
            std::string errors;
            std::istringstream response_stream(response);

            if (Json::parseFromStream(reader, response_stream, &root, &errors)) {
                std::cout << "Parsed Order Response: " << root << std::endl;
            } else {
                throw std::runtime_error("Failed to parse the JSON response: " + errors);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error placing order: " << e.what() << std::endl;
        }
    }

    void get_positions() {
        try {
            if (get_token().empty()) {
                throw std::runtime_error("Authentication failed");
            }

            std::string currency, kind;
            std::cout << "Enter currency (e.g., BTC): ";
            std::cin >> currency;
            std::cout << "Enter kind (e.g., future): ";
            std::cin >> kind;

            std::string params = "currency=" + currency + "&kind=" + kind;
            std::string response = make_request("private/get_positions", params, true);

            Json::Value root;
            Json::CharReaderBuilder reader;
            std::string errors;
            std::istringstream response_stream(response);

            if (Json::parseFromStream(reader, response_stream, &root, &errors)) {
                if (root.isMember("result")) {
                    std::cout << "Positions result: " << root["result"] << std::endl;
                } else {
                    std::cout << "No result field found in the response." << std::endl;
                }
            } else {
                throw std::runtime_error("Failed to parse the JSON response: " + errors);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error getting positions: " << e.what() << std::endl;
        }
    }

    void get_order_book() {
        string instrument_name;
        int depth;
        
        cout << "Enter instrument name (e.g., BTC-PERPETUAL): ";
        cin >> instrument_name;
        cout << "Enter depth (e.g., 5): ";
        cin >> depth;

        string order_book_url = "https://test.deribit.com/api/v2/public/get_order_book?instrument_name=" + instrument_name + "&depth=" + to_string(depth);
        
        CURL* curl;
        CURLcode res;
        string readBuffer;

        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, order_book_url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
        }

        cout << "Order book response: " << readBuffer << endl;

        Json::CharReaderBuilder reader;
        Json::Value root;
        string errors;

        std::istringstream s(readBuffer);
        if (Json::parseFromStream(reader, s, &root, &errors)) {
            cout << "Parsed JSON Order Book: " << root << endl;
        } else {
            cout << "Failed to parse the JSON response: " << errors << endl;
        }
    }

    void modify_order() {
        try {
            if (get_token().empty()) {
                throw std::runtime_error("Authentication failed");
            }

            std::string order_id;
            int amount;
            double price;
            std::string advanced;

            std::cout << "Enter order ID: ";
            std::cin >> order_id;
            std::cout << "Enter amount: ";
            std::cin >> amount;
            std::cout << "Enter price: ";
            std::cin >> price;
            std::cout << "Enter advanced type (e.g., implv): ";
            std::cin >> advanced;

            std::string params = "order_id=" + order_id + 
                                 "&amount=" + std::to_string(amount) + 
                                 "&price=" + std::to_string(price);
            // Uncomment the following line if you want to include the advanced parameter
            // params += "&advanced=" + advanced;

            std::string response = make_request("private/edit", params, true);

            std::cout << "Modify order response: " << response << std::endl;

            Json::Value root;
            Json::CharReaderBuilder reader;
            std::string errors;
            std::istringstream response_stream(response);

            if (Json::parseFromStream(reader, response_stream, &root, &errors)) {
                std::cout << "Parsed Modify Order Response: " << root << std::endl;
            } else {
                throw std::runtime_error("Failed to parse the JSON response: " + errors);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error modifying order: " << e.what() << std::endl;
        }
    }

    void cancel_order() {
        try {
            if (get_token().empty()) {
                throw std::runtime_error("Authentication failed");
            }

            std::string order_id;

            std::cout << "Enter order ID to cancel: ";
            std::cin >> order_id;

            std::string params = "order_id=" + order_id;
            std::string response = make_request("private/cancel", params, true);

            std::cout << "Cancel order response: " << response << std::endl;

            Json::Value root;
            Json::CharReaderBuilder reader;
            std::string errors;
            std::istringstream response_stream(response);

            if (Json::parseFromStream(reader, response_stream, &root, &errors)) {
                std::cout << "Parsed Cancel Order Response: " << root << std::endl;
            } else {
                throw std::runtime_error("Failed to parse the JSON response: " + errors);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error canceling order: " << e.what() << std::endl;
        }
    }
};

void display_menu() {
    cout << "Select an action:" << endl;
    cout << "1. Authenticate" << endl;
    cout << "2. Place Order" << endl;
    cout << "3. Get Positions" << endl;
    cout << "4. Get Order Book" << endl;
    cout << "5. Modify Order" << endl;
    cout << "6. Cancel Order" << endl;
    cout << "7. Exit" << endl;
}

int main() {
    DeribitAPI api;
    int choice;

    const std::map<int, std::pair<std::string, std::function<void()>>> actions = {
        {1, {"Authenticating...", [&]() { std::cout << "Token: " << api.get_token() << std::endl; }}},
        {2, {"Placing Order...", [&]() { api.place_order(); }}},
        {3, {"Fetching positions...", [&]() { api.get_positions(); }}},
        {4, {"Fetching order book...", [&]() { api.get_order_book(); }}},
        {5, {"Modifying order...", [&]() { api.modify_order(); }}},
        {6, {"Canceling order...", [&]() { api.cancel_order(); }}},
        {7, {"Exiting...", []() {}}}
    };

    do {
        display_menu();
        std::cin >> choice;

        auto it = actions.find(choice);
        if (it != actions.end()) {
            std::cout << it->second.first << std::endl;
            it->second.second();
        } else {
            std::cout << "Invalid option, please try again." << std::endl;
        }

    } while (choice != 7);

    return 0;
}
