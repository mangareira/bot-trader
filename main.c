#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <openssl/hmac.h>
#include <cjson/cJSON.h>

#define API_URL "https://api.coinex.com/v2"

struct BufferStruct
{
    char* buffer;
    size_t size;
};

static size_t WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data)
{
    size_t realsize = size * nmemb;
    struct BufferStruct* mem = (struct BufferStruct*) data;

    char *new_buffer = mem->buffer ? realloc(mem->buffer, mem->size + realsize + 1) : malloc(realsize + 1);
    if (!new_buffer) {
        free(mem->buffer);
        mem->buffer = NULL;
        mem->size = 0;
        return 0;
    }

    mem->buffer = new_buffer;
    memcpy(&(mem->buffer[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = '\0';

    return realsize;
}

void hmac_sha256(const char *key, const char *data, char *output, size_t output_size) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(), key, strlen(key), (unsigned char *)data, strlen(data), hash, &len);

    for (unsigned int i = 0; i < len && i * 2 + 1 < output_size; i++) {
        snprintf(output + i * 2, 3, "%02x", hash[i]);
    }
}

void filter_and_save_json(const char *input, const char *filter_key, struct BufferStruct *chunk) {
    cJSON *json = cJSON_Parse(input);
    if (!json) {
        fprintf(stderr, "Erro ao analisar JSON.\n");
        return ;
    }

    cJSON *data = cJSON_GetObjectItem(json, "data");
    if (!cJSON_IsArray(data)) {
        fprintf(stderr, "Erro: 'data2' não é uma lista.\n");
        cJSON_Delete(json);
        return ;
    }

    cJSON *filtered_data = cJSON_CreateArray();

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, data) {
        cJSON *market = cJSON_GetObjectItem(item, "market");
        cJSON *last_price = cJSON_GetObjectItem(item, "last");
        char *endptr;
        double last_price_num = strtod(last_price->valuestring, &endptr);
        if (cJSON_IsString(market) && strstr(market->valuestring, filter_key) && !strstr(market->valuestring, "INDEX") && last_price_num > 0.00000333 ) {
            cJSON_AddItemToArray(filtered_data, cJSON_CreateString(market->valuestring));
        }
    }

    char *filtered_json = cJSON_PrintUnformatted(filtered_data);
    free(chunk->buffer);
    chunk->buffer = strdup(filtered_json);
    chunk->size = strlen(filtered_json);

    free(filtered_json);
    cJSON_Delete(filtered_data);
    cJSON_Delete(json);

}

int is_red_candle(cJSON *candle) {
    double open = atof(cJSON_GetObjectItem(candle, "open")->valuestring);
    double close = atof(cJSON_GetObjectItem(candle, "close")->valuestring);

    return close < open;
}

int bullish_engulfment(const char* input) {
    cJSON *json = cJSON_Parse(input);
    cJSON *data = cJSON_GetObjectItem(json, "data");
    if (!cJSON_IsArray(data)) {
        fprintf(stderr, "Erro: 'data1' não é uma lista.\n");
        cJSON_Delete(json);
        return 0;
    }

    cJSON *previous_candle = cJSON_GetArrayItem(data, cJSON_GetArraySize(data) - 2);
    cJSON *current_candle = cJSON_GetArrayItem(data, cJSON_GetArraySize(data) - 1);

    double prev_open = atof(cJSON_GetObjectItem(previous_candle, "open")->valuestring);
    double prev_close = atof(cJSON_GetObjectItem(previous_candle, "close")->valuestring);
    double curr_open = atof(cJSON_GetObjectItem(current_candle, "open")->valuestring);
    double curr_close = atof(cJSON_GetObjectItem(current_candle, "close")->valuestring);

    if (is_red_candle(previous_candle) && !is_red_candle(current_candle) &&
        curr_open <= prev_close && curr_close > prev_open) {
        cJSON_Delete(json);
        return 1; 
    }

    cJSON_Delete(json);
    return 0;

}

void create_obj_coin(cJSON *item, cJSON *array, char* coin, double founds) {
    cJSON *price = cJSON_GetObjectItem(item, "last");
    const char *price_str = price->valuestring;
    char *endptr;
    double priceCoin = strtod(price_str, &endptr);
    
    double limit = priceCoin * 1.03;
    int buffer_limit = snprintf(NULL, 0, "%.8f", limit) + 1;
    char *priceLimit = (char*)malloc(buffer_limit);  
    snprintf(priceLimit, buffer_limit, "%.8f", limit);
    
    double stop = priceCoin * 0.953;
    int buffer_stop = snprintf(NULL, 0, "%.8f", stop) + 1;
    char *priceStop = (char*)malloc(buffer_stop);  
    snprintf(priceStop, buffer_stop, "%.8f", stop);
    
    double stopLimit = priceCoin * 0.951;
    int buffer_stopLimit = snprintf(NULL, 0, "%.8f", stopLimit) + 1;
    char *priceStopLimit = (char*)malloc(buffer_stopLimit);  
    snprintf(priceStopLimit, buffer_stopLimit, "%.8f", stopLimit);

    int quantity = founds / (priceCoin * 0.99);

    cJSON *coinObject = cJSON_CreateObject();
    cJSON_AddStringToObject(coinObject, "coin", coin);
    cJSON_AddStringToObject(coinObject, "price", price_str);
    cJSON_AddStringToObject(coinObject, "priceLimit", priceLimit);
    cJSON_AddStringToObject(coinObject, "priceStop", priceStop);
    cJSON_AddStringToObject(coinObject, "priceStopLimit", priceStopLimit);
    cJSON_AddNumberToObject(coinObject, "quantity", quantity);
    cJSON_AddItemToArray(array, coinObject);
    free(priceLimit);
    free(priceStop);
    free(priceStopLimit);
}

void get_info_coin(CURL *curl,const char* buffer, char *coin, CURLcode res, struct BufferStruct *payload) {
                    
    struct BufferStruct chunck = { NULL, 0 };                
    char url_index[128];
    sprintf(url_index, "%s/spot/ticker?market=%s", API_URL, coin);
    curl_easy_setopt(curl, CURLOPT_URL, url_index );
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunck);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Erro ao buscar índice para %s: %s\n", coin, curl_easy_strerror(res));
    }
    free(payload->buffer);
    payload->buffer = strdup(chunck.buffer);
    payload->size = chunck.size;
    free(chunck.buffer);
    chunck.buffer = NULL;
    chunck.size = 0;
}

void add_obj_to_array(const char* buffer, cJSON *array, char* coin, double founds) {
    cJSON *index = cJSON_Parse(buffer);
    if (!index) {
        fprintf(stderr, "Erro ao analisar JSON.\n");
        cJSON_Delete(index);
    }  
    cJSON *index_data = cJSON_GetObjectItem(index, "data");
    
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, index_data) {
        create_obj_coin(item, array, coin, founds);
    }
    // criação da ordem de compra mercado e venda
    // create_order();
    cJSON_Delete(index);
}

int compare_avg(const void *a, const void *b) {
    cJSON *objA = *(cJSON **)a;
    cJSON *objB = *(cJSON **)b;

    // Obtém os valores de "avg" de cada objeto
    double avgA = cJSON_GetObjectItem(objA, "avg")->valuedouble;
    double avgB = cJSON_GetObjectItem(objB, "avg")->valuedouble;

    if (avgA < avgB) return -1;
    if (avgA > avgB) return 1;
    return 0;
}

void setAvg(cJSON *data, cJSON *array, int arraySize) {
    for (int i = 0; i < arraySize; i++) {
        cJSON *candle = cJSON_GetArrayItem(data, i);
        
        cJSON *close = cJSON_GetObjectItem(candle, "close");
        const char *close_str = close->valuestring;
        char *endptrClose;
        double close_num = strtod(close_str, &endptrClose);

        cJSON *open = cJSON_GetObjectItem(candle, "open");
        const char *open_str = open->valuestring;
        char *endptrOpen;
        double open_num = strtod(open_str, &endptrOpen);
        
        double avg = (close_num + open_num) / 2;
        cJSON *objAvg = cJSON_CreateObject();
        cJSON_AddNumberToObject(objAvg, "avg", avg);
        cJSON_AddItemToArray(array,objAvg);
    }
}

int rising(const char* buffer) {
    cJSON *json = cJSON_Parse(buffer);

    if (!json) {
        fprintf(stderr, "Erro ao analisar JSON.\n");
        return 0;
    }  

    cJSON *data = cJSON_GetObjectItem(json, "data");
    int arraySize = cJSON_GetArraySize(data);
    cJSON *candleAvg = cJSON_CreateArray();

    setAvg(data, candleAvg, arraySize);
    int candleAvgSize = cJSON_GetArraySize(candleAvg);
    cJSON *avgItemNew = cJSON_GetArrayItem(candleAvg, 3);
    cJSON *avgItemOld = cJSON_GetArrayItem(candleAvg, 0);

    double newAvg_num = cJSON_GetObjectItem(avgItemNew, "avg")->valuedouble;
    double oldAvg_num = cJSON_GetObjectItem(avgItemOld, "avg")->valuedouble;

    cJSON_Delete(candleAvg);
    cJSON_Delete(json);

    return (newAvg_num > oldAvg_num) ? 1 : 0;
}

int is_rising_mediun(const char* coin, CURL *curl) {
    struct BufferStruct chunck = {NULL, 0};
    chunck.buffer = malloc(1);
    chunck.size = 0;

    char url[128];
    sprintf(url, "%s/spot/kline?market=%s&limit=4&period=%s", API_URL, coin, "3day");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunck);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Erro ao buscar candles para %s: %s\n", coin, curl_easy_strerror(res));
    } 
    if(rising(chunck.buffer)) {
        free(chunck.buffer);
        chunck.size = 0;
        return 1;
    }
    free(chunck.buffer);
    chunck.size = 0;
    return 0;
}

void sort(cJSON* candleAvg) {
    int size = cJSON_GetArraySize(candleAvg);
    cJSON **tempArray = (cJSON **)malloc(size * sizeof(cJSON *));
    
    for (int i = 0; i < size; i++) {
        tempArray[i] = cJSON_DetachItemFromArray(candleAvg, 0);
    }

    qsort(tempArray, size, sizeof(cJSON *), compare_avg);

    for (int i = 0; i < size; i++) {
        cJSON_AddItemToArray(candleAvg, tempArray[i]);
    }

    free(tempArray);
}

void remove_candles(cJSON* candleAvg) {
    for(int i = 0; i < 3; i++) {
        cJSON_DeleteItemFromArray(candleAvg, i);
    }
    for(int i = 1; i <=3; i++) {
        cJSON_DeleteItemFromArray(candleAvg, cJSON_GetArraySize(candleAvg) - i);
    }
}

int check_below(const char* buffer) {

    cJSON *json = cJSON_Parse(buffer);

    if (!json) {
        fprintf(stderr, "Erro ao analisar JSON.\n");
        return 0;
    }  

    cJSON *data = cJSON_GetObjectItem(json, "data");
    int arraySize = cJSON_GetArraySize(data);
    cJSON *candleAvg = cJSON_CreateArray();
    
    cJSON* item_array = cJSON_GetArrayItem(data, 32);
    cJSON* price = cJSON_GetObjectItem(item_array, "close");
    const char *price_str = price->valuestring;
    char *endptr;
    double price_num = strtod(price_str, &endptr);

    cJSON* price_open = cJSON_GetObjectItem(item_array, "open");
    const char *price_open_str = price_open->valuestring;
    char *endptrOpen;
    double price_open_num = strtod(price_open_str, &endptrOpen);

    setAvg(data, candleAvg, arraySize);
    sort(candleAvg);
    remove_candles(candleAvg);

    double sum;

    for(int i = 0; i < cJSON_GetArraySize(candleAvg);i++) {
        cJSON* item = cJSON_GetArrayItem(candleAvg, i);
        cJSON* avg = cJSON_GetObjectItem(item, "avg");
        sum = sum + avg->valuedouble;
    }

    double resist = (sum / cJSON_GetArraySize(candleAvg));

    cJSON_Delete(json);
    return ((price_num < resist) && ((((price_num - price_open_num)/ price_open_num) * 100) < 3.3) && ((((price_num - price_open_num)/ price_open_num) * 100) > -9.9));
}

int check_if_is_below(const char* coin, CURL *curl) {
    struct BufferStruct chunck = {NULL, 0};
    chunck.buffer = malloc(1);
    chunck.size = 0;

    char url[128];
    sprintf(url, "%s/spot/kline?market=%s&limit=33&period=%s", API_URL, coin, "2hour");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunck);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Erro ao buscar candles para %s: %s\n", coin, curl_easy_strerror(res));
    } 

    if(check_below(chunck.buffer)){
        free(chunck.buffer);
        chunck.size = 0;
        return 1;
    }
    free(chunck.buffer);
    chunck.size = 0;
    return 0;
}

int is_rising_long(const char* coin, CURL *curl) {

    struct BufferStruct chunck = {NULL, 0};
    chunck.buffer = malloc(1);
    chunck.size = 0;

    char url[128];
    sprintf(url, "%s/spot/kline?market=%s&limit=5&period=%s", API_URL, coin, "1week");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunck);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Erro ao buscar candles para %s: %s\n", coin, curl_easy_strerror(res));
    } 
    if(rising(chunck.buffer)) {
        free(chunck.buffer);
        chunck.size = 0;
        return 1;
    }
    free(chunck.buffer);
    chunck.size = 0;
    return 0;

}

void create_order() {}

void buy_or_notbuy(
    const char *input, 
    const char *interval, 
    CURL *curl, 
    struct BufferStruct chunck, 
    struct BufferStruct *payload,
    double founds
) {
    cJSON *json = cJSON_Parse(input);
    if (!json) {
        fprintf(stderr, "Erro ao analisar JSON.\n");
        return;
    }  

    chunck.buffer = malloc(1);
    chunck.size = 0;

    cJSON *market = NULL;
    cJSON *coinBuy = cJSON_CreateArray();
    cJSON_ArrayForEach(market, json) {
        char url[256];

        sprintf(url, "%s/spot/kline?market=%s&limit=2&period=%s", API_URL, market->valuestring, interval);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunck);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "Erro ao buscar candles para %s: %s\n", market->valuestring, curl_easy_strerror(res));
        } else {
            if (strlen(chunck.buffer) > 0 && bullish_engulfment(chunck.buffer)) {
                if(strlen(chunck.buffer) > 0 && is_rising_mediun(market->valuestring, curl)) {
                    if(strlen(chunck.buffer) > 0 && check_if_is_below(market->valuestring, curl)) {
                        if(strlen(chunck.buffer) > 0 && is_rising_long(market->valuestring, curl)) {
                            get_info_coin(curl, chunck.buffer, market->valuestring, res, &chunck);
                            add_obj_to_array(chunck.buffer, coinBuy, market->valuestring, founds);
                        }
                    }
                }
            }
        }
        free(chunck.buffer);
        chunck.buffer = malloc(1);
        chunck.size = 0;
    }
    char *coin = cJSON_PrintUnformatted(coinBuy);
    free(chunck.buffer);
    chunck.buffer = NULL;
    chunck.size = 0;
    free(payload->buffer);
    payload->buffer = strdup(coin);
    payload->size = strlen(coin);
    free(coin);
    cJSON_Delete(coinBuy);
    cJSON_Delete(json);

}


int main() {
    CURL *curl = curl_easy_init();

    char* api_id = getenv("COINEX_API_KEY");
    char* api_secret = getenv("COINEX_SECRET_KEY");

    double founds = 0.03;

    char url[256];
    sprintf(url, "%s/spot/ticker", API_URL);

    // struct curl_slist *headers = NULL;

    // char timestamp[32] ;
    // sprintf(timestamp, "%lu", time(NULL) * 1000);

    // char prepared_string[128];
    // sprintf(prepared_string, "GET/v2/spot/market%s", timestamp);

    // char hmac_result[128];
    // hmac_sha256(api_secret, prepared_string, hmac_result, sizeof(hmac_result));

    // char api_id_header[64];
    // sprintf(api_id_header, "X-COINEX-KEY: %s",api_id );

    // char sign_header[256];
    // sprintf(sign_header, "X-COINEX-SIGN: %s", hmac_result);

    // char timestamp_header[64];
    // sprintf(timestamp_header, "X-COINEX-TIMESTAMP: %s", timestamp);

    struct BufferStruct chunck = {NULL, 0};


    while(1){
        if(curl) {    
            chunck.buffer = malloc(1);
            chunck.size = 0;
            CURLcode res;
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunck);
            res = curl_easy_perform(curl);
            if(res != CURLE_OK){
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            }
            else {
                filter_and_save_json(chunck.buffer, "BTC", &chunck);
                buy_or_notbuy(chunck.buffer, "4hour", curl, chunck, &chunck, founds);
                
                FILE *file;
                file = fopen("data.json", "a");
                fprintf(file, "%s", chunck.buffer);
                fclose(file);
                printf("%lu bytes retrieved and saved to data.json\n", (unsigned long)chunck.size);
            }
            free(chunck.buffer);
            chunck.size = 0;
        }
    }
    curl_easy_cleanup(curl);
    return 0;
}
