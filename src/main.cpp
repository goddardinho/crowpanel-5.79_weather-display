/**
 * A weather forecast display system
 * using the Elecrow CrowPanel ESP32 E-Paper HMI 5.79-inch Display,
 * showing every 3 hours forecast for the next 12 hours.
 * 
 * The weather forecast data is obtained using the OpenWeatherMap API.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "EPD.h"
#include "icons.h"
#include "config.h"
#include "../test/testdata.h" // data for offline test
#include <map>

//=============================================================================
// Constants
//=============================================================================

// Test Settings
const bool TEST_MODE = false;      // Test mode (uses test data instead of the API when set to true)

// Display Settings
const size_t FORECAST_COUNT = 5;   // Number of forecast periods to display

// E-Paper Settings
const int EPD_BUFFER_SIZE = 27200; // Size of E-Paper display buffer

//=============================================================================
// Type Definitions
//=============================================================================

/**
 * Weather icon enumeration
 * Maps to indices in the Weather_Num array in icons.h
 */
enum WeatherIconNumber {
  ICON_CLEAR_DAY = 0,
  ICON_CLEAR_NIGHT = 1,
  ICON_CLOUDS = 2,
  ICON_RAIN = 3,
  ICON_THUNDERSTORM = 4,
  ICON_SNOW = 5,
  ICON_MIST = 6
};

/**
 * Forecast information structure
 * Stores weather forecast data for a specific time period
 */
struct ForecastInfo {
  String time;       // Time (HH:MM)
  int iconNumber;    // Weather icon number
  float temperature; // Temperature (C or F depending on TEMPERATURE_UNIT)
  float pop;         // Probability of precipitation (%)
};

// Forward declarations for header layout helpers used before their definitions.
int getCenteredTextX(const String& text, uint16_t fontSize, int areaStartX, int areaWidth);
void drawBoldLine(int x1, int y1, int x2, int y2);
int getRightAlignedX(const String& text, uint16_t fontSize, int rightEdge);

//=============================================================================
// Global Constants
//=============================================================================

/**
 * Weather mapping table
 * Maps OpenWeatherMap icon codes to our icon enumeration
 */
std::map<String, WeatherIconNumber> WEATHER_MAPPINGS = {
  {"01d", ICON_CLEAR_DAY},
  {"01n", ICON_CLEAR_NIGHT},
  {"02d", ICON_CLOUDS},
  {"02n", ICON_CLOUDS},
  {"03d", ICON_CLOUDS},
  {"03n", ICON_CLOUDS},
  {"04d", ICON_CLOUDS},
  {"04n", ICON_CLOUDS},
  {"09d", ICON_RAIN},
  {"09n", ICON_RAIN},
  {"10d", ICON_RAIN},
  {"10n", ICON_RAIN},
  {"11d", ICON_THUNDERSTORM},
  {"11n", ICON_THUNDERSTORM},
  {"13d", ICON_SNOW},
  {"13n", ICON_SNOW},
  {"50d", ICON_MIST},
  {"50n", ICON_MIST}
};

//=============================================================================
// Global Variables
//=============================================================================

// E-Paper Display Buffer
uint8_t ImageBW[EPD_BUFFER_SIZE];

// API Related Variables
String jsonBuffer;
int httpResponseCode = 0;

// Array to Store Forecast Data
ForecastInfo hourlyForecasts[FORECAST_COUNT];

// Metadata shown in the display header
String displayLocation;
String displayDayDate;
String displayPressure;
String displayWind;
char displayPressureTrend = '-';
int timezoneOffsetSeconds = TIMEZONE_OFFSET * 3600;
int currentPressure = 1013;
float currentWindSpeed = 0.0f;
float currentWindDeg = 0.0f;
String currentWindCardinal = "N";
RTC_DATA_ATTR int previousPressure = 0;
RTC_DATA_ATTR bool hasPreviousPressure = false;


//=============================================================================
// Deep-sleep Functions
//=============================================================================

/**
 * Function to Enter Deep-Sleep Mode
 * 
 * @param wakeup true if it needs to wake up later
 */
void enterDeepSleep(bool wakeup) {
  Serial.println("Entering Deep-sleep mode. Will wake up later.");
  Serial.flush();
  
  // Put EPD in Sleep Mode Before Entering Deep-Sleep Mode
  EPD_DeepSleep();

  delay(4000);
  
  // Enter Deep-Sleep Mode
  if (wakeup) {
    // Wake Up After n minutes (default: 60 minites)
    esp_sleep_enable_timer_wakeup(INTERVAL_IN_MINUTES * 60UL * 1000UL * 1000); // microseconds
  }
  esp_deep_sleep_start();
}

/**
 * Converts wind direction in degrees to an eight-point cardinal direction.
 *
 * @param windDeg Wind direction in meteorological degrees.
 * @return Cardinal direction, such as N or SW.
 */
String getCardinalDirection(float windDeg) {
  static const char* directions[] = {
    "N", "NE", "E", "SE", "S", "SW", "W", "NW"
  };
  float normalized = fmodf(windDeg, 360.0f);
  if (normalized < 0) {
    normalized += 360.0f;
  }
  const int index = (int)roundf(normalized / 45.0f) % 8;
  return String(directions[index]);
}

/**
 * Builds display header metadata (location and day/date)
 *
 * @param doc Parsed weather JSON document
 */
void buildDisplayMetadata(const JsonDocument& doc) {
  const char* timezone = doc["timezone"] | "Local";

  #ifdef LOCATION_NAME
    if (strlen(LOCATION_NAME) > 0) {
      displayLocation = String(LOCATION_NAME);
    } else {
      displayLocation = String(timezone);
    }
  #else
    displayLocation = String(timezone);
  #endif

  // Use API timezone offset when available (helps with DST-aware formatting).
  timezoneOffsetSeconds = doc["timezone_offset"] | (TIMEZONE_OFFSET * 3600);

  time_t currentLocalTime = (time_t) (doc["current"]["dt"].as<long>() + timezoneOffsetSeconds);
  struct tm *timeInfo = gmtime(&currentLocalTime);

  char dayDateBuffer[24];
  strftime(dayDateBuffer, sizeof(dayDateBuffer), "%a %b %d", timeInfo);
  displayDayDate = String(dayDateBuffer);

  currentPressure = doc["current"]["pressure"].as<int>();
  if (hasPreviousPressure) {
    if (currentPressure > previousPressure) {
      displayPressureTrend = '^';
    } else if (currentPressure < previousPressure) {
      displayPressureTrend = 'v';
    } else {
      displayPressureTrend = '-';
    }
  }
  previousPressure = currentPressure;
  hasPreviousPressure = true;

  const float pressureInHg = currentPressure * 0.0295299831f;
  char pressureBuffer[24];
  snprintf(pressureBuffer, sizeof(pressureBuffer), "P %.2f inHg", pressureInHg);
  displayPressure = String(pressureBuffer);

  currentWindSpeed = doc["current"]["wind_speed"].as<float>();
  currentWindDeg = doc["current"]["wind_deg"].as<float>();
  currentWindCardinal = getCardinalDirection(currentWindDeg);
  const char* windUnit = TEMPERATURE_UNIT == 0 ? "m/s" : "mph";
  char windBuffer[32];
  snprintf(windBuffer, sizeof(windBuffer), "Wind %.1f %s", currentWindSpeed, windUnit);
  displayWind = String(windBuffer) + " " + currentWindCardinal;
}

/**
 * Calculates x-position for right-aligned text.
 *
 * EPD_ShowString advances by size/2 pixels per character.
 *
 * @param text Text to align
 * @param fontSize Font size passed to EPD_ShowString
 * @param rightEdge Right-most x coordinate to align against
 * @return x-position to pass to EPD_ShowString
 */
int getRightAlignedX(const String& text, uint16_t fontSize, int rightEdge) {
  const int charWidth = fontSize / 2;
  int x = rightEdge - (text.length() * charWidth);
  return x < 0 ? 0 : x;
}

/**
 * Calculates x-position for centered text inside a fixed-width area.
 *
 * EPD_ShowString advances by size/2 pixels per character.
 *
 * @param text Text to center
 * @param fontSize Font size passed to EPD_ShowString
 * @param areaStartX Left x-coordinate of the area
 * @param areaWidth Width of the area
 * @return x-position to pass to EPD_ShowString
 */
int getCenteredTextX(const String& text, uint16_t fontSize, int areaStartX, int areaWidth) {
  const int charWidth = fontSize / 2;
  const int textWidth = text.length() * charWidth;
  int x = areaStartX + ((areaWidth - textWidth) / 2);
  return x < areaStartX ? areaStartX : x;
}

/**
 * Draws a bold pressure trend arrow.
 *
 * @param centerX Center x-coordinate for the arrow.
 * @param topY Top y-coordinate for the arrow.
 * @param trend Arrow direction: ^, v, or -.
 */
void drawPressureTrendArrow(int centerX, int topY, char trend) {
  const int shaftX = centerX;
  const int arrowTop = topY + 2;
  const int arrowBottom = topY + 22;
  const int headWidth = 9;

  if (trend == '^') {
    drawBoldLine(shaftX, arrowBottom, shaftX, arrowTop);
    drawBoldLine(shaftX - headWidth, arrowTop + headWidth, shaftX, arrowTop);
    drawBoldLine(shaftX + headWidth, arrowTop + headWidth, shaftX, arrowTop);
  } else if (trend == 'v') {
    drawBoldLine(shaftX, arrowTop, shaftX, arrowBottom);
    drawBoldLine(shaftX - headWidth, arrowBottom - headWidth, shaftX, arrowBottom);
    drawBoldLine(shaftX + headWidth, arrowBottom - headWidth, shaftX, arrowBottom);
  } else {
    drawBoldLine(shaftX - 12, topY + 13, shaftX + 12, topY + 13);
    drawBoldLine(shaftX - 12, topY + 13, shaftX - 5, topY + 6);
    drawBoldLine(shaftX + 12, topY + 13, shaftX + 5, topY + 6);
  }
}

void drawBoldLine(int x1, int y1, int x2, int y2) {
  const float lineX = (float)(x2 - x1);
  const float lineY = (float)(y2 - y1);
  const float lineLength = sqrtf((lineX * lineX) + (lineY * lineY));
  if (lineLength == 0.0f) {
    return;
  }

  const int offsetX = (int)roundf(-lineY / lineLength);
  const int offsetY = (int)roundf(lineX / lineLength);
  const int strokeWidth = 4;
  const int firstOffset = -(strokeWidth / 2);
  for (int stroke = 0; stroke < strokeWidth; stroke++) {
    const int offset = firstOffset + stroke;
    EPD_DrawLine(x1 + (offsetX * offset), y1 + (offsetY * offset),
                 x2 + (offsetX * offset), y2 + (offsetY * offset), BLACK);
  }
}

/**
 * Draws a bold wind-direction arrow from the API degree value.
 *
 * @param centerX Center x-coordinate for the arrow.
 * @param topY Top y-coordinate for the arrow.
 * @param degrees Meteorological wind direction in degrees.
 */
void drawWindDirectionArrow(int centerX, int topY, float degrees) {
  const int length = 20;
  const int headLength = 9;
  const float radians = degrees * PI / 180.0f;
  const int centerY = topY + 22;
  const int tipX = centerX + (int)roundf(sinf(radians) * length);
  const int tipY = centerY - (int)roundf(cosf(radians) * length);
  const float headAngle = 0.65f;

  // Use a single-ended shaft so the arrowhead is the only pointed side.
  drawBoldLine(centerX, centerY, tipX, tipY);

  const int leftHeadX = tipX - (int)roundf(sinf(radians - headAngle) * headLength);
  const int leftHeadY = tipY + (int)roundf(cosf(radians - headAngle) * headLength);
  const int rightHeadX = tipX - (int)roundf(sinf(radians + headAngle) * headLength);
  const int rightHeadY = tipY + (int)roundf(cosf(radians + headAngle) * headLength);
  drawBoldLine(leftHeadX, leftHeadY, tipX, tipY);
  drawBoldLine(rightHeadX, rightHeadY, tipX, tipY);
}

//=============================================================================
// Display Functions
//=============================================================================

/**
 * Displays weather forecast on the E-Paper display
 * 
 * Renders time, weather icon, temperature and probability of precipitation
 * for each forecast period in a column layout
 */
void displayWeatherForecast()
{
  const int textBufferSize = 40;    // Size of text buffer for formatting
  const int displayWidth = 792;     // Effective drawable width in pixels
  const int forecastStartX = 0;
  const int columnWidth = displayWidth / FORECAST_COUNT;
  const int contentTopY = 54;
  char buffer[textBufferSize];

  // Initialize Display
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();

  // Display metadata header

  // Center header text across the full display width.
  const int locationX = getCenteredTextX(displayLocation, 24, 0, displayWidth);
  EPD_ShowString(locationX, 4, (char*)displayLocation.c_str(), 24, BLACK);

  memset(buffer, 0, sizeof(buffer));
  snprintf(buffer, sizeof(buffer), "%s", displayDayDate.c_str());
  const int dayDateX = getCenteredTextX(String(buffer), 24, 0, displayWidth);
  EPD_ShowString(dayDateX, 28, buffer, 24, BLACK);

  const int windX = 2;
  const int windTextWidth = displayWind.length() * (24 / 2);
  const int windCenterX = windX + (windTextWidth / 2);
  drawWindDirectionArrow(windCenterX, 0, currentWindDeg);
  EPD_ShowString(windX, 28, (char*)displayWind.c_str(), 24, BLACK);

  const int pressureX = getRightAlignedX(displayPressure, 24, displayWidth - 2);
  const int pressureTextWidth = displayPressure.length() * (24 / 2);
  const int pressureCenterX = pressureX + (pressureTextWidth / 2);
  drawPressureTrendArrow(pressureCenterX, 0, displayPressureTrend);
  EPD_ShowString(pressureX, 28, (char*)displayPressure.c_str(), 24, BLACK);

  EPD_DrawLine(0, contentTopY, 791, contentTopY, BLACK);

  // Display Each Forecast Data
  for (int i = 0; i < FORECAST_COUNT; i++) {
    if (hourlyForecasts[i].time.length() > 0) {
      // Calculate x position for this column
      int baseX = forecastStartX + columnWidth * i;
      
      // Display Time
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", hourlyForecasts[i].time.c_str());
      EPD_ShowString(getCenteredTextX(String(buffer), 24, baseX, columnWidth), 58, buffer, 24, BLACK);

      // Display Weather Icon
      const int iconX = baseX + ((columnWidth - 128) / 2);
      EPD_ShowPicture(iconX, 82, 128, 128, Weather_Num[hourlyForecasts[i].iconNumber], WHITE);

      // Display Temperature with appropriate unit
      memset(buffer, 0, sizeof(buffer));
      if (TEMPERATURE_UNIT == 0) {
        snprintf(buffer, sizeof(buffer), "%3d C", (int)round(hourlyForecasts[i].temperature));
      } else {
        snprintf(buffer, sizeof(buffer), "%3d F", (int)round(hourlyForecasts[i].temperature));
      }
      const int temperatureX = getCenteredTextX(String(buffer), 24, baseX, columnWidth);
      EPD_ShowString(temperatureX, 212, buffer, 24, BLACK);
      EPD_DrawCircle(temperatureX + 44, 220, 2, BLACK, false);
      EPD_DrawCircle(temperatureX + 44, 220, 3, BLACK, false);

      if (i != 0) {
        // Display Probability of precipitation
        memset(buffer, 0, sizeof(buffer));
        snprintf(buffer, sizeof(buffer), "%3d %%", (int)round(100 * hourlyForecasts[i].pop));
        EPD_ShowString(getCenteredTextX(String(buffer), 24, baseX, columnWidth), 238, buffer, 24, BLACK);
      }
    }
  }

  // Draw Separator Lines
  for (int i = 1; i < FORECAST_COUNT; i++) {
    int separatorX = forecastStartX + columnWidth * i;
    EPD_DrawLine(separatorX, contentTopY, separatorX, 271, BLACK);
  }

  // Update Display
  EPD_Display(ImageBW);
  EPD_PartUpdate();
  
  Serial.println("Weather forecast displayed successfully");
}

/**
 * Displays an error message on the E-Paper display
 * 
 * @param message Error message to display
 */
void displayErrorMessage(const char* message) {
  Serial.print("ERROR: ");
  Serial.println(message);

  // Initialize Display
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();

  // Display Error Message
  EPD_ShowString(30, 30, "ERROR:", 24, BLACK);
  
  // Process to add line breaks every 56 characters
  const int maxCharsPerLine = 56;
  const int lineHeight = 30;
  int currentLine = 0;
  int messageLength = strlen(message);
  int startPos = 0;
  
  while (startPos < messageLength) {
    char lineBuffer[maxCharsPerLine + 1];
    int charsToDisplay = messageLength - startPos;
    
    // Determine the number of characters to display in this line (maximum 56)
    if (charsToDisplay > maxCharsPerLine) {
      charsToDisplay = maxCharsPerLine;
    }
    
    // Copy the line text to buffer
    strncpy(lineBuffer, message + startPos, charsToDisplay);
    lineBuffer[charsToDisplay] = '\0'; // Add null terminator
    
    // Display the current line
    EPD_ShowString(60, 70 + (lineHeight * currentLine), lineBuffer, 24, BLACK);
    
    // Prepare for the next line
    startPos += charsToDisplay;
    currentLine++;
  }

  // Update Display
  EPD_Display(ImageBW);
  EPD_PartUpdate();
}

//=============================================================================
// Network Functions
//=============================================================================

/**
 * Connects to WiFi network using credentials from config.h
 * 
 * @return true if connection successful, false if failed
 */
bool connectToWiFi() {
  Serial.print("Connecting to WiFi network: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Connection Timeout (180 seconds)
  const int wifiTimeoutMs = 180000;
  const int delayMs = 5000;
  int attempts = wifiTimeoutMs / delayMs;
  
  // Wait until Connection is Complete
  while (WiFi.status() != WL_CONNECTED && attempts > 0) {
    delay(delayMs);
    Serial.print(".");
    attempts--;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Connected to WiFi with IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("");
    Serial.println("Failed to connect to WiFi");
    return false;
  }
}

/**
 * Disconnects from WiFi to save power
 */
void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disconnected for power saving");
}

/**
 * Sends an HTTP GET request to the specified URL
 * 
 * @param url URL to send the request to
 * @return Response content as String
 */
String httpGETRequest(const char* url) {
  WiFiClient client;
  HTTPClient http;

  // Initialize HTTP Client and Specify Server URL
  http.begin(client, url);

  // Set HTTP Request Timeout
  const int httpTimeoutMs = 20000; // HTTP request timeout in milliseconds
  http.setTimeout(httpTimeoutMs);

  Serial.print("Sending HTTP GET request to: ");
  Serial.println(url);

  // Send HTTP GET Request
  httpResponseCode = http.GET();

  // Initialize Response Content
  String payload = "{}";

  // Check Response Code and Process Response Content
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    
    if (httpResponseCode == HTTP_CODE_OK) {
      // Copy Response to Buffer
      payload = http.getString();
    } else {
      Serial.println("Request failed with non-200 status code");
    }
  } else {
    Serial.print("HTTP Request failed, error code: ");
    Serial.println(httpResponseCode);
  }
  
  // Release HTTP Client Resources
  http.end();
  
  return payload;
}

//=============================================================================
// Weather Forecast Data Processing Functions
//=============================================================================

/**
 * Maps OpenWeatherMap icon code to our internal icon number
 * 
 * @param OpenWeatherMapIcon OpenWeatherMap icon code (e.g., "01d")
 * @return Corresponding internal icon number
 */
int getWeatherIconNum(String OpenWeatherMapIcon) {
  // Search for a Match from the Mapping Table
  if (WEATHER_MAPPINGS.find(OpenWeatherMapIcon) != WEATHER_MAPPINGS.end()) {
    return WEATHER_MAPPINGS[OpenWeatherMapIcon];
  }
  
  // Default is Cloudy if no match found
  Serial.println("Warning: No icon match found for " + OpenWeatherMapIcon + ", using default");
  return ICON_THUNDERSTORM;
}

/**
 * Stores weather information in the forecast array
 * 
 * @param index Index in the forecast array
 * @param unixTime Unix timestamp (UTC)
 * @param iconCode OpenWeatherMap icon code
 * @param temperature Temperature
 * @param pop Probability of precipitation
 */
void storeWeatherInfo(int index, long unixTime, String iconCode, float temperature, float pop) {
  if (index < 0 || index >= FORECAST_COUNT) {
    displayErrorMessage("Invalid forecast index");
    enterDeepSleep(true);
    return;
  }

  // Convert UTC Unix Timestamp to Local Time
  time_t localTime = unixTime + timezoneOffsetSeconds;
  struct tm *timeinfo = gmtime(&localTime);

  // Store Information in Structure
  char tempTimeStr[6];
  strftime(tempTimeStr, sizeof(tempTimeStr), "%k:%M", timeinfo);
  hourlyForecasts[index].time = String(tempTimeStr);
  hourlyForecasts[index].iconNumber = getWeatherIconNum(iconCode);
  hourlyForecasts[index].temperature = temperature;
  hourlyForecasts[index].pop = pop;
}

/**
 * Prints weather forecast data to Serial for debugging
 */
void printWeatherData() {
  Serial.println("\n--- Weather Forecast Data ---");
  for (int i = 0; i < FORECAST_COUNT; i++) {
    if (hourlyForecasts[i].time.length() > 0) {
      Serial.print("Time: ");
      Serial.print(hourlyForecasts[i].time);
      Serial.print(" | IconNumber: ");
      Serial.print(hourlyForecasts[i].iconNumber);
      Serial.print(" | Temperature: ");
      Serial.print(hourlyForecasts[i].temperature);
      Serial.print("C | POP: ");
      Serial.print(hourlyForecasts[i].pop);
      Serial.println("%");
    }
  }
}

/**
 * Fetches weather forecast data from OpenWeatherMap API or test data
 * 
 * @param useTestData If true, uses test data instead of API
 * @return JSON string containing weather data, or empty string on failure
 */
String fetchWeatherData(bool useTestData = TEST_MODE) {
  // Returns Test Data When in test mode
  if (useTestData) {
    Serial.println("Using test data instead of API");
    return String(TEST_WEATHER_DATA);
  }
  
  // Check WiFi Connection Status
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected. Attempting to reconnect...");
    if (!connectToWiFi()) {
      Serial.println("WiFi reconnection failed");
      return "";
    }
  }

  // Build OpenWeatherMap API Request URL
  String url = "http://api.openweathermap.org";
  url += "/data/3.0/onecall";
  url += "?lat=" + String((float) LATITUDE, 5);
  url += "&lon=" + String((float) LONGITUDE, 5);
  url += "&units=" + String(TEMPERATURE_UNIT == 0 ? "metric" : "imperial") + "&lang=en&exclude=minutely,daily,alerts";
  url += "&appid=" + (String) OPENWEATHERMAP_API_KEY;

  Serial.println("Fetching weather forecast data from OpenWeatherMap...");
  Serial.print("OpenWeatherMap URL: ");
  Serial.println(url);
  
  // Send HTTP Request and Get Response
  int retryCount = 0;
  bool exitLoop = false;
  String responseData = "";
  
  const int maxRetryCount = 3;     // Maximum number of retry attempts
  while (retryCount < maxRetryCount && !exitLoop) {
    responseData = httpGETRequest(url.c_str());

    switch (httpResponseCode) {
      case HTTP_CODE_OK:
        exitLoop = true;
        break;
      case HTTP_CODE_BAD_REQUEST:
        exitLoop = true;
        displayErrorMessage("Either some mandatory parameters in the request are missing or some of request parameters have incorrect format or values out of allowed range.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_UNAUTHORIZED:
        exitLoop = true;
        displayErrorMessage("API token did not providen in the request or in case API token provided in the request does not grant access to this API.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_NOT_FOUND:
        exitLoop = true;
        displayErrorMessage("Data with requested parameters (lat, lon, date etc) does not exist in service database.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_TOO_MANY_REQUESTS:
        exitLoop = true;
        displayErrorMessage("Key quota of requests for provided API to this API was exceeded.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_INTERNAL_SERVER_ERROR:
      case HTTP_CODE_BAD_GATEWAY:
      case HTTP_CODE_SERVICE_UNAVAILABLE:
      case HTTP_CODE_GATEWAY_TIMEOUT:
      case HTTPC_ERROR_READ_TIMEOUT:
        Serial.println("Unexpected Error.");
        retryCount++;
        Serial.print("Retry attempt ");
        Serial.print(retryCount);
        Serial.print(" of ");
        Serial.println(maxRetryCount);
        delay(60000);
        break;
      default:
        exitLoop = true;
        char numchar[10];
        std::sprintf(numchar, "Unknown Error. httpResponseCode: %d", httpResponseCode);
        displayErrorMessage(numchar);
        enterDeepSleep(false);
        break;
    }
  }
  
  if (!exitLoop) {
    displayErrorMessage("Failed to fetch weather forecast data after multiple attempts");
    enterDeepSleep(false);
    return "";
  }
  
  return responseData;
}

/**
 * Analyzes weather data JSON and stores it in the forecast array
 * 
 * @param jsonData JSON string containing weather data
 * @return true if analysis was successful, false otherwise
 */
bool analyzeWeatherData(const String& jsonData) {
  // Parse JSON Data
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonData);

  // Check if JSON Parsing was Successful
  if (error != DeserializationError::Ok) {
    Serial.print("JSON parsing failed! Error: ");
    Serial.println(error.c_str());
    String errorMsg = "JSON parsing failed! Error: " + String(error.c_str());
    displayErrorMessage(errorMsg.c_str());
    return false;
  }

  // Store Current Weather Information
  buildDisplayMetadata(doc);

  // Store Current Weather Information
  storeWeatherInfo(0, doc["current"]["dt"], 
                  doc["current"]["weather"][0]["icon"],
                  doc["current"]["temp"].as<float>(),
                  doc["current"]["pop"].as<float>());

  // Store Future Weather Information (3, 6, 9, 12 hours later)
  const int hourlyIndices[] = {3, 6, 9, 12};
  
  for (int i = 0; i < 4; i++) {
    int hourlyIndex = hourlyIndices[i];
    
    if (hourlyIndex < doc["hourly"].size()) {
      storeWeatherInfo(i + 1, doc["hourly"][hourlyIndex]["dt"], 
                      doc["hourly"][hourlyIndex]["weather"][0]["icon"],
                      doc["hourly"][hourlyIndex]["temp"].as<float>(), 
                      doc["hourly"][hourlyIndex]["pop"].as<float>());
    } else {
      Serial.print("Warning: Hourly index ");
      Serial.print(hourlyIndex);
      Serial.println(" is out of range");
    }
  }

  // Display Retrieved Data on Serial Monitor
  printWeatherData();
  
  Serial.println("Weather forecast data analyzed successfully");
  return true;
}

/**
 * Fetches and analyzes weather forecast data
 * 
 * Retrieves current weather and hourly forecast data,
 * then stores it in the forecast array
 */
void fetchAndAnalyzeWeatherData() {
  // Fetch weather data
  String jsonData = fetchWeatherData();
  
  // Check if data was fetched successfully
  if (jsonData.isEmpty()) {
    return;
  }
  
  // Store JSON data in global buffer for potential debugging
  jsonBuffer = jsonData;
  
  // Analyze the weather data
  if (!analyzeWeatherData(jsonData)) {
    enterDeepSleep(true);
  }
}

/**
 * Function to Perform Initialization
 */
void setup() {
  // Initialize Serial Communication
  Serial.begin(115200);
  Serial.println("Weather Forecast Display System Starting...");

  // Set E-Paper Display Power Pin
  const int epdPowerPin = 7;     // GPIO pin for E-Paper power control
  pinMode(epdPowerPin, OUTPUT);
  digitalWrite(epdPowerPin, HIGH);

  // Initialize E-Paper Display GPIO
  EPD_GPIOInit();
  
  // Only connect to WiFi if not in test mode
  if (!TEST_MODE) {
    // WiFi Connection
    if (!connectToWiFi()) {
      displayErrorMessage("WiFi Connection Error");
      enterDeepSleep(false);
      return;
    }
  }
  
  // Fetch and Analyze Weather forecast Data
  fetchAndAnalyzeWeatherData();
  
  // Only disconnect WiFi if not in test mode
  if (!TEST_MODE) {
    // Disconnect WiFi (Power Saving)
    disconnectWiFi();
  }

  // Display Weather Forecast
  displayWeatherForecast();
  
  // Enter Deep-Sleep Mode
  enterDeepSleep(true);
}

/**
 * Main Loop Function
 * Not Executed When Using Deep-Sleep Mode
 */
void loop() {
}
