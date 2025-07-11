#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <time.h>
#include <ArduinoJson.h>

const char *ssid = "Act4";
const char *password = "Awesome2003";

WebServer server(80);

// Pin Definitions
const int relayPin = 16;
const int currentSensorPin = 36;

// Schedule settings
bool scheduleEnabled = false;
time_t scheduleStart;
time_t scheduleEnd;
unsigned long durationSeconds = 0;
bool useDuration = false;
time_t durationStartTime = 0;

// Smart pump algorithm settings
struct SmartPumpConfig
{
  float noLoadCurrent = 0.5;              // Current threshold below which pump is considered running dry
  float loadCurrent = 1.2;                // Current threshold above which water is detected
  unsigned long testRunTime = 60;         // Test run duration in seconds (1 min)
  unsigned long waitTime = 600;           // Wait time between tests in seconds (10 min)
  unsigned long maxTestTime = 8 * 3600;   // Maximum testing time (8 hours from start)
  unsigned long targetRunTime = 2 * 3600; // Target run time when water is available (2 hours)
} smartConfig;

// Smart pump state
enum PumpState
{
  IDLE,
  NORMAL_RUN,
  TEST_RUN,
  WAITING,
  COMPLETED
};

struct SmartPumpStatus
{
  PumpState state = IDLE;
  time_t sessionStartTime = 0;
  time_t lastTestTime = 0;
  time_t waterDetectedTime = 0;
  time_t currentStateStartTime = 0;
  unsigned long totalRunTimeWithLoad = 0;
  unsigned long totalRunTimeNoLoad = 0;
  bool waterPresent = false;
  int testAttempts = 0;
  String lastLogMessage = "";
} smartStatus;

// Pump state
bool pumpOn = false;

// Current measurement
float currentValue = 0.0;
float powerWatts = 0.0;

void togglePump(bool state)
{
  pumpOn = state;
  digitalWrite(relayPin, state ? LOW : HIGH);     // Assuming LOW = Relay ON
  Serial.println(state ? "Pump ON" : "Pump OFF"); // Fixed the message
}

void readCurrent()
{
  const int samples = 1000;
  long totalADC = 0;

  // First pass: Measure actual DC bias
  for (int i = 0; i < samples; i++)
  {
    totalADC += analogRead(currentSensorPin);
  }
  float averageADC = totalADC / (float)samples;
  float biasVoltage = averageADC * (3.3 / 4095.0);

  // Second pass: Measure AC component around the bias
  long sumOfSquares = 0;
  for (int i = 0; i < samples; i++)
  {
    int adcValue = analogRead(currentSensorPin);
    float voltage = adcValue * (3.3 / 4095.0);
    float centeredVoltage = voltage - biasVoltage;
    sumOfSquares += centeredVoltage * centeredVoltage * 1e6;
  }

  float rmsVoltage = sqrt(sumOfSquares / samples) / 1000.0;
  float calibrationFactor = 30.0;
  currentValue = rmsVoltage * calibrationFactor;
  powerWatts = currentValue * 230.0;
}

bool isWaterPresent()
{
  readCurrent();
  return currentValue >= smartConfig.loadCurrent;
}

bool isPumpRunningDry()
{
  readCurrent();
  return currentValue <= smartConfig.noLoadCurrent;
}

void logPumpActivity(String message)
{
  smartStatus.lastLogMessage = message;
  Serial.println("[SMART PUMP] " + message);

  // Add timestamp
  struct tm timeinfo;
  if (getLocalTime(&timeinfo))
  {
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    smartStatus.lastLogMessage = String(timeStr) + " - " + message;
  }
}

void startSmartSchedule()
{
  time_t now;
  time(&now);

  smartStatus.state = TEST_RUN;
  smartStatus.sessionStartTime = now;
  smartStatus.currentStateStartTime = now;
  smartStatus.totalRunTimeWithLoad = 0;
  smartStatus.totalRunTimeNoLoad = 0;
  smartStatus.testAttempts = 0;
  smartStatus.waterPresent = false;

  togglePump(true);
  logPumpActivity("Smart schedule started - Initial water test");
}

void updateSmartPump() {
  if (!scheduleEnabled || smartStatus.state == IDLE) return;
  
  time_t now;
  time(&now);
  
  unsigned long stateElapsed = now - smartStatus.currentStateStartTime;
  unsigned long sessionElapsed = now - smartStatus.sessionStartTime;
  
  // Check if we've exceeded maximum test time - this is the hard limit
  if (sessionElapsed >= smartConfig.maxTestTime) {
    smartStatus.state = COMPLETED;
    scheduleEnabled = false;
    togglePump(false);
    logPumpActivity("Maximum test time (" + String(smartConfig.maxTestTime/3600) + " hours) reached. Session ended.");
    return;
  }
  
  switch (smartStatus.state) {
    case TEST_RUN:
      if (stateElapsed >= smartConfig.testRunTime) {
        // Test run complete, check if water is present
        if (isWaterPresent()) {
          // Water detected! Switch to normal run
          smartStatus.state = NORMAL_RUN;
          smartStatus.waterDetectedTime = now;
          smartStatus.waterPresent = true;
          smartStatus.currentStateStartTime = now;
          
          // DYNAMIC EXTENSION LOGIC
          time_t remainingMaxTime = smartConfig.maxTestTime - sessionElapsed;
          time_t desiredRunTime = smartConfig.targetRunTime; // 2 hours default
          
          // Extend schedule end time, but respect maxTestTime constraint
          if (now >= scheduleEnd) {
            // We're past original schedule end time
            time_t extensionTime = min(desiredRunTime, remainingMaxTime);
            scheduleEnd = now + extensionTime;
            logPumpActivity("Water detected after original schedule! Extended by " + 
                          String(extensionTime/60) + " minutes (Max time constraint: " + 
                          String(remainingMaxTime/60) + " min remaining)");
          } else {
            // We're still within original schedule time
            time_t currentScheduleRemaining = scheduleEnd - now;
            if (currentScheduleRemaining < desiredRunTime) {
              // Extend to reach desired run time, but respect maxTestTime
              time_t extensionNeeded = desiredRunTime - currentScheduleRemaining;
              time_t actualExtension = min(extensionNeeded, remainingMaxTime - currentScheduleRemaining);
              if (actualExtension > 0) {
                scheduleEnd += actualExtension;
                logPumpActivity("Extended schedule by " + String(actualExtension/60) + 
                              " minutes to achieve target runtime");
              }
            }
            logPumpActivity("Water detected! Starting pumping cycle");
          }
          
          smartStatus.totalRunTimeNoLoad += smartConfig.testRunTime;
        } else {
          // No water, go to waiting state
          smartStatus.state = WAITING;
          smartStatus.currentStateStartTime = now;
          smartStatus.testAttempts++;
          togglePump(false);
          
          smartStatus.totalRunTimeNoLoad += smartConfig.testRunTime;
          logPumpActivity("Test " + String(smartStatus.testAttempts) + 
                         " - No water detected, waiting " + String(smartConfig.waitTime/60) + 
                         " minutes (Session time: " + String(sessionElapsed/60) + "/" + 
                         String(smartConfig.maxTestTime/60) + " min)");
        }
      }
      break;
      
    case WAITING:
      if (stateElapsed >= smartConfig.waitTime) {
        // Start another test
        smartStatus.state = TEST_RUN;
        smartStatus.currentStateStartTime = now;
        togglePump(true);
        logPumpActivity("Starting test " + String(smartStatus.testAttempts + 1) + 
                       " (Session time: " + String(sessionElapsed/60) + "/" + 
                       String(smartConfig.maxTestTime/60) + " min)");
      }
      break;
      
    case NORMAL_RUN:
      // Check if water is still present
      if (isPumpRunningDry()) {
        // Water stopped, go back to testing mode
        smartStatus.state = TEST_RUN;
        smartStatus.currentStateStartTime = now;
        smartStatus.waterPresent = false;
        
        // Update total run time with load
        if (smartStatus.waterDetectedTime > 0) {
          smartStatus.totalRunTimeWithLoad += (now - smartStatus.waterDetectedTime);
        }
        
        logPumpActivity("Water supply interrupted after " + 
                       String((now - smartStatus.waterDetectedTime)/60) + 
                       " minutes, resuming test mode");
      } else {
        // Water still present, check if we've reached target runtime
        unsigned long runTimeWithWater = now - smartStatus.waterDetectedTime;
        if (runTimeWithWater >= smartConfig.targetRunTime) {
          // Target runtime achieved, complete successfully
          smartStatus.state = COMPLETED;
          scheduleEnabled = false;
          togglePump(false);
          smartStatus.totalRunTimeWithLoad += runTimeWithWater;
          logPumpActivity("SUCCESS: Target runtime achieved - " + 
                         String(smartConfig.targetRunTime/60) + " minutes with water. " +
                         "Total session time: " + String(sessionElapsed/60) + " minutes");
        }
        
        // Check if extended schedule end time is reached
        else if (now >= scheduleEnd) {
          smartStatus.state = COMPLETED;
          scheduleEnabled = false;
          togglePump(false);
          smartStatus.totalRunTimeWithLoad += runTimeWithWater;
          logPumpActivity("Extended schedule ended - Total runtime with water: " + 
                         String(runTimeWithWater/60) + " minutes. " +
                         "Total session time: " + String(sessionElapsed/60) + " minutes");
        }
      }
      break;
      
    case COMPLETED:
      // Ensure pump is off and schedule is disabled
      if (pumpOn) {
        togglePump(false);
      }
      scheduleEnabled = false;
      break;
  }
}

void handleRoot()
{
  // Enhanced HTML with smart pump status
  String html = R"HTMLPAGE(<!DOCTYPE html>
<html>
<head>
<link type="text/css" rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0-alpha3/dist/css/bootstrap.min.css"/>
    <title>Smart ESP32 Pump Controller</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta charset="UTF-8">
    <script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/3.9.1/chart.min.js"></script>
    <style>
        body { 
            font-family: Inter, Roboto, "Helvetica Neue", Arial, sans-serif;
            margin: 20px; 
            background-color: #ffffff; 
        }
        .container { 
            max-width: 1200px; 
            margin: 0 auto; 
            background-color: #F6F0F0; 
            padding: 20px; 
            border-radius: 10px; 
            box-shadow: 0 2px 10px rgba(0,0,0,0.1); 
        }
        .status { 
            padding: 15px; 
            margin: 10px 0; 
            border-radius: 8px; 
            text-align: center;
            font-weight: bold;
        }
        .on { 
            background-color: #d4edda; 
            color: #155724; 
            border: 2px solid #c3e6cb;
        }
        .off { 
            background-color: #f8d7da; 
            color: #721c24; 
            border: 2px solid #f1b0b7;
        }
        .smart-status {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            border-radius: 10px;
            margin: 15px 0;
        }
        .smart-grid {
            display: grid;
            grid-template-columns: 1fr 1fr 1fr;
            gap: 15px;
            margin-top: 15px;
        }
        .smart-metric {
            background: rgba(255,255,255,0.1);
            padding: 10px;
            border-radius: 8px;
            text-align: center;
        }
        button { 
            padding: 12px 24px; 
            margin: 8px; 
            font-size: 16px; 
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
        }
        .btn-primary { 
            background-color: #3D90D7; 
            color: white;
            box-shadow: 2px 2px 2px 3px rgba(0, 0, 255, 0.2);
        }
        .btn-primary:hover { 
            background-color: #201E43; 
        }
        .btn-success { 
            background-color: #28a745; 
            color: white; 
        }
        .btn-danger { 
            background-color: #dc3545; 
            color: white; 
        }
        input { 
            padding: 10px; 
            margin: 5px; 
            border: 1px solid #ddd;
            border-radius: 4px;
            font-size: 14px;
        }
        .sensor-data { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px; 
            border-radius: 10px; 
            margin: 15px 0;
            text-align: center;
        }
        .sensor-value {
            font-size: 2em;
            font-weight: bold;
            margin: 10px 0;
        }
        .chart-container {
            background: white;
            padding: 20px;
            border-radius: 10px;
            margin: 20px 0;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
        }
        .control-section {
            background: #f8f9fa;
            padding: 20px;
            border-radius: 10px;
            margin: 15px 0;
            display: flex;
            flex-direction: column;
            align-items: center;
        }
        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
        }
        @media (max-width: 768px) {
            .grid { grid-template-columns: 1fr; }
            .smart-grid { grid-template-columns: 1fr; }
        }
        #duration{width: 80%;}
        .log-area {
            background: #f8f9fa;
            border: 1px solid #ddd;
            border-radius: 8px;
            padding: 15px;
            height: 150px;
            overflow-y: auto;
            font-family: monospace;
            font-size: 12px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1 style="text-align: center; color: #333;">🧠 Smart ESP32 Pump Controller</h1>
        
        <div id="status" class="status off">
            <h3>Pump Status: <span id="pump-status">OFF</span></h3>
        </div>
        
        <div class="smart-status">
            <h3>🤖 Smart Pump Intelligence</h3>
            <div><strong>State:</strong> <span id="smart-state">IDLE</span></div>
            <div><strong>Water Status:</strong> <span id="water-status">Unknown</span></div>
            <div class="smart-grid">
                <div class="smart-metric">
                    <div><strong>Test Attempts</strong></div>
                    <div id="test-attempts">0</div>
                </div>
                <div class="smart-metric">
                    <div><strong>Runtime (Water)</strong></div>
                    <div id="runtime-load">0 min</div>
                </div>
                <div class="smart-metric">
                    <div><strong>Runtime (No Water)</strong></div>
                    <div id="runtime-noload">0 min</div>
                </div>
            </div>
            <div style="margin-top: 15px;">
                <strong>Last Activity:</strong> <span id="last-log">No activity</span>
            </div>
        </div>
        
        <div class="grid">
            <div class="sensor-data">
                <h3>⚡ Current Reading</h3>
                <div class="sensor-value"><span id="current">0.00</span> A</div>
            </div>
            <div class="sensor-data">
                <h3>💡 Power Consumption</h3>
                <div class="sensor-value"><span id="power">0.00</span> W</div>
            </div>
        </div>
        
        <div class="chart-container">
            <h3>📊 Real-time Current Monitor</h3>
            <canvas id="currentChart" width="400" height="200"></canvas>
            <div style="margin-top: 10px;display:flex;align-items:center;justify-content: space-around;">
                <div style="display:flex; flex-wrap: wrap">
                <button class="btn-primary" onclick="clearChart()">Clear Data</button>
                <button class="btn-success" onclick="toggleAutoUpdate()" id="auto-btn">Auto Update: ON</button>
                </div>
            </div>
        </div>
        
        <div class="control-section">
            <h3>🎛️ Manual Control</h3>
            <button id="toggle-btn" class="btn-primary" onclick="togglePump()">Toggle Pump</button>
            <button class="btn-success" onclick="updateSensorData()">Refresh Data</button>
        </div>
        
        <div class="grid">
            <div class="control-section">
                <h3>⏱️ Smart Duration Schedule</h3>
                <input type="number" id="duration" placeholder="Duration in seconds" min="1" max="14400">
                <br>
                <button class="btn-primary" onclick="scheduleSmartDuration()">Start Smart Duration</button>
            </div>
            
            <div class="control-section">
                <h3>🕐 Smart Time Schedule</h3>
                <input type="time" id="start-time">
                <input type="time" id="end-time">
                <br>
                <button class="btn-primary" onclick="scheduleSmartTime()">Set Smart Schedule</button>
            </div>
        </div>
        
        <div class="control-section">
            <h3>⚙️ Smart Pump Configuration</h3>
            <div class="grid">
                <div>
                    <label>No Load Current (A):</label>
                    <input type="number" id="no-load-current" step="0.1" value="0.5">
                </div>
                <div>
                    <label>Load Current (A):</label>
                    <input type="number" id="load-current" step="0.1" value="1.2">
                </div>
                <div>
                    <label>Test Run Time (sec):</label>
                    <input type="number" id="test-time" value="60">
                </div>
                <div>
                    <label>Wait Time (sec):</label>
                    <input type="number" id="wait-time" value="600">
                </div>
                <div>
                    <label>Max Test Time (sec):</label>
                    <input type="number" id="max-test-time" value="28800">
                </div>
            </div>
            <button class="btn-primary" onclick="updateSmartConfig()">Update Configuration</button>
        </div>
    </div>

    <script>
        // Chart.js setup (same as before)
        const ctx = document.getElementById('currentChart').getContext('2d');
        const currentChart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: [],
                datasets: [{
                    label: 'Current (A)',
                    data: [],
                    borderColor: 'rgb(75, 192, 192)',
                    backgroundColor: 'rgba(75, 192, 192, 0.2)',
                    tension: 0.4,
                    fill: true
                }, {
                    label: 'Power (W/10)',
                    data: [],
                    borderColor: 'rgb(255, 99, 132)',
                    backgroundColor: 'rgba(255, 99, 132, 0.2)',
                    tension: 0.4,
                    fill: false
                }]
            },
            options: {
                responsive: true,
                scales: {
                    y: {
                        beginAtZero: true,
                        title: {
                            display: true,
                            text: 'Current (A) / Power (W/10)'
                        }
                    },
                    x: {
                        title: {
                            display: true,
                            text: 'Time'
                        }
                    }
                },
                plugins: {
                    legend: {
                        display: true,
                        position: 'top'
                    }
                },
                animation: {
                    duration: 500
                }
            }
        });

        let autoUpdate = true;
        const maxDataPoints = 50;

        function addDataToChart(current, power) {
            const now = new Date();
            const timeLabel = now.getHours().toString().padStart(2, '0') + ':' + 
                            now.getMinutes().toString().padStart(2, '0') + ':' +
                            now.getSeconds().toString().padStart(2, '0');
            
            currentChart.data.labels.push(timeLabel);
            currentChart.data.datasets[0].data.push(current);
            currentChart.data.datasets[1].data.push(power / 10);
            
            if (currentChart.data.labels.length > maxDataPoints) {
                currentChart.data.labels.shift();
                currentChart.data.datasets[0].data.shift();
                currentChart.data.datasets[1].data.shift();
            }
            
            currentChart.update('none');
        }

        function clearChart() {
            currentChart.data.labels = [];
            currentChart.data.datasets[0].data = [];
            currentChart.data.datasets[1].data = [];
            currentChart.update();
        }

        function toggleAutoUpdate() {
            autoUpdate = !autoUpdate;
            document.getElementById('auto-btn').textContent = 'Auto Update: ' + (autoUpdate ? 'ON' : 'OFF');
            document.getElementById('auto-btn').className = autoUpdate ? 'btn-success' : 'btn-danger';
        }

        function updateStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    const status = document.getElementById('pump-status');
                    const statusDiv = document.getElementById('status');
                    
                    if (data.pumpOn) {
                        status.textContent = 'ON';
                        statusDiv.className = 'status on';
                    } else {
                        status.textContent = 'OFF';
                        statusDiv.className = 'status off';
                    }
                    
                    // Update smart status
                    if (data.smartStatus) {
                        document.getElementById('smart-state').textContent = data.smartStatus.state || 'IDLE';
                        document.getElementById('water-status').textContent = data.smartStatus.waterPresent ? 'Present' : 'Not Detected';
                        document.getElementById('test-attempts').textContent = data.smartStatus.testAttempts || 0;
                        document.getElementById('runtime-load').textContent = Math.floor((data.smartStatus.totalRunTimeWithLoad || 0) / 60) + ' min';
                        document.getElementById('runtime-noload').textContent = Math.floor((data.smartStatus.totalRunTimeNoLoad || 0) / 60) + ' min';
                        document.getElementById('last-log').textContent = data.smartStatus.lastLogMessage || 'No activity';
                    }
                })
                .catch(error => console.log('Status update error:', error));
        }
        
        function updateSensorData() {
            fetch('/sensor')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('current').textContent = data.current.toFixed(2);
                    document.getElementById('power').textContent = data.power.toFixed(1);
                    
                    if (autoUpdate) {
                        addDataToChart(data.current, data.power);
                    }
                })
                .catch(error => console.log('Sensor update error:', error));
        }
        
        function togglePump() {
            fetch('/toggle', { method: 'POST' })
                .then(response => response.text())
                .then(result => {
                    console.log('Toggle result:', result);
                    updateStatus();
                })
                .catch(error => console.log('Toggle error:', error));
        }
        
        function scheduleSmartDuration() {
            const duration = document.getElementById('duration').value;
            if (!duration || duration < 1) {
                alert('Please enter a valid duration (1-14400 seconds)');
                return;
            }
            
            fetch('/smart-schedule', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ duration: parseInt(duration), smart: true })
            })
            .then(response => response.text())
            .then(result => {
                alert('Smart duration scheduled: ' + result);
                updateStatus();
            })
            .catch(error => console.log('Schedule error:', error));
        }
        
        function scheduleSmartTime() {
            const startTime = document.getElementById('start-time').value;
            const endTime = document.getElementById('end-time').value;
            
            if (!startTime || !endTime) {
                alert('Please enter both start and end times');
                return;
            }
            
            fetch('/smart-schedule', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ start: startTime, end: endTime, smart: true })
            })
            .then(response => response.text())
            .then(result => {
                alert('Smart time scheduled: ' + result);
                updateStatus();
            })
            .catch(error => console.log('Schedule error:', error));
        }
        
        function updateSmartConfig() {
            const config = {
                noLoadCurrent: parseFloat(document.getElementById('no-load-current').value),
                loadCurrent: parseFloat(document.getElementById('load-current').value),
                testRunTime: parseInt(document.getElementById('test-time').value),
                waitTime: parseInt(document.getElementById('wait-time').value),
                maxTestTime: parseInt(document.getElementById('max-test-time').value)
            };
            
            fetch('/smart-config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(config)
            })
            .then(response => response.text())
            .then(result => {
                alert('Configuration updated: ' + result);
            })
            .catch(error => console.log('Config update error:', error));
        }
        
        // Auto-update every 2 seconds
        setInterval(() => {
            updateStatus();
            updateSensorData();
        }, 2000);
        
        // Initial load
        updateStatus();
        updateSensorData();
    </script>
</body>
</html>)HTMLPAGE";

  server.send(200, "text/html", html);
}
void handleSchedule() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No JSON body provided");
    return;
  }

  String body = server.arg("plain");
  Serial.println("Received schedule: " + body);
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON format");
    return;
  }

  if (doc.containsKey("duration")) {
    int duration = doc["duration"].as<int>();
    if (duration <= 0 || duration > 14400) {
      server.send(400, "text/plain", "Duration must be between 1 and 14400 seconds");
      return;
    }
    
    durationSeconds = duration;
    useDuration = true;
    scheduleEnabled = true;
    durationStartTime = 0;
    // Reset smart status for simple duration mode
    smartStatus.state = IDLE;
    Serial.println("Simple duration schedule set: " + String(durationSeconds) + " seconds");
  } 
  else if (doc.containsKey("start") && doc.containsKey("end")) {
    String startTime = doc["start"].as<String>();
    String endTime = doc["end"].as<String>();
    
    if (startTime.length() != 5 || endTime.length() != 5) {
      server.send(400, "text/plain", "Invalid time format. Use HH:MM");
      return;
    }
    
    int startHour = startTime.substring(0, 2).toInt();
    int startMin = startTime.substring(3, 5).toInt();
    int endHour = endTime.substring(0, 2).toInt();
    int endMin = endTime.substring(3, 5).toInt();
    
    // Validate time ranges
    if (startHour < 0 || startHour > 23 || startMin < 0 || startMin > 59 ||
        endHour < 0 || endHour > 23 || endMin < 0 || endMin > 59) {
      server.send(400, "text/plain", "Invalid time values");
      return;
    }
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      server.send(500, "text/plain", "Failed to obtain time");
      return;
    }
    
    struct tm startTm = timeinfo;
    startTm.tm_hour = startHour;
    startTm.tm_min = startMin;
    startTm.tm_sec = 0;
    
    struct tm endTm = timeinfo;
    endTm.tm_hour = endHour;
    endTm.tm_min = endMin;
    endTm.tm_sec = 0;
    
    scheduleStart = mktime(&startTm);
    scheduleEnd = mktime(&endTm);
    
    if (scheduleEnd <= scheduleStart) {
      endTm.tm_mday += 1;
      scheduleEnd = mktime(&endTm);
    }
    
    useDuration = false;
    scheduleEnabled = true;
    smartStatus.state = IDLE;
    
    Serial.println("Simple time schedule set: " + startTime + " to " + endTime);
  }
  else {
    server.send(400, "text/plain", "Invalid schedule format. Provide either 'duration' or 'start'+'end'");
    return;
  }
  
  server.send(200, "text/plain", "Schedule set successfully");
}
void handleToggle() {
  pumpOn = !pumpOn;
  togglePump(pumpOn);
  
  // Disable schedule when manually toggling and reset smart pump state
  if (scheduleEnabled) {
    scheduleEnabled = false;
    smartStatus.state = IDLE;
    smartStatus.sessionStartTime = 0;
    smartStatus.currentStateStartTime = 0;
    smartStatus.waterDetectedTime = 0;
    smartStatus.totalRunTimeWithLoad = 0;
    smartStatus.totalRunTimeNoLoad = 0;
    smartStatus.testAttempts = 0;
    smartStatus.waterPresent = false;
    logPumpActivity("Schedule disabled due to manual override");
  }
  
  server.send(200, "text/plain", pumpOn ? "ON" : "OFF");
}
void handleSmartSchedule()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "text/plain", "No JSON body provided");
    return;
  }

  String body = server.arg("plain");
  Serial.println("Received smart schedule: " + body);

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);

  if (error)
  {
    server.send(400, "text/plain", "Invalid JSON format");
    return;
  }

  if (doc.containsKey("duration"))
  {
    int duration = doc["duration"].as<int>();
    if (duration <= 0 || duration > 14400)
    {
      server.send(400, "text/plain", "Duration must be between 1 and 14400 seconds");
      return;
    }

    durationSeconds = duration;
    useDuration = true;
    scheduleEnabled = true;

    // Set schedule end time for smart pump
    time_t now;
    time(&now);
    scheduleEnd = now + durationSeconds;

    startSmartSchedule();
    Serial.println("Smart duration schedule set: " + String(durationSeconds) + " seconds");
  }
  else if (doc.containsKey("start") && doc.containsKey("end"))
  {
    String startTime = doc["start"].as<String>();
    String endTime = doc["end"].as<String>();

    if (startTime.length() != 5 || endTime.length() != 5)
    {
      server.send(400, "text/plain", "Invalid time format. Use HH:MM");
      return;
    }

    // Parse time strings
    int startHour = startTime.substring(0, 2).toInt();
    int startMin = startTime.substring(3, 5).toInt();
    int endHour = endTime.substring(0, 2).toInt();
    int endMin = endTime.substring(3, 5).toInt();

    // Validate time ranges
    if (startHour < 0 || startHour > 23 || startMin < 0 || startMin > 59 ||
        endHour < 0 || endHour > 23 || endMin < 0 || endMin > 59)
    {
      server.send(400, "text/plain", "Invalid time values");
      return;
    }

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
      server.send(500, "text/plain", "Failed to obtain time");
      return;
    }

    struct tm startTm = timeinfo;
    startTm.tm_hour = startHour;
    startTm.tm_min = startMin;
    startTm.tm_sec = 0;

    struct tm endTm = timeinfo;
    endTm.tm_hour = endHour;
    endTm.tm_min = endMin;
    endTm.tm_sec = 0;

    scheduleStart = mktime(&startTm);
    scheduleEnd = mktime(&endTm);

    // Handle next day end time
    if (scheduleEnd <= scheduleStart)
    {
      endTm.tm_mday += 1;
      scheduleEnd = mktime(&endTm);
    }

    useDuration = false;
    scheduleEnabled = true;

    // Check if we should start immediately
    time_t now;
    time(&now);
    if (now >= scheduleStart && now < scheduleEnd)
    {
      startSmartSchedule();
      Serial.println("Smart time schedule started immediately: " + startTime + " to " + endTime);
    }
    else
    {
      Serial.println("Smart time schedule set: " + startTime + " to " + endTime);
    }
  }
  else
  {
    server.send(400, "text/plain", "Invalid schedule format. Provide either 'duration' or 'start'+'end'");
    return;
  }

  server.send(200, "text/plain", "Smart schedule set successfully");
}

void handleSmartConfig()
{
  if (!server.hasArg("plain"))
  {
    server.send(400, "text/plain", "No JSON body provided");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);

  if (error)
  {
    server.send(400, "text/plain", "Invalid JSON format");
    return;
  }

  if (doc.containsKey("noLoadCurrent"))
  {
    smartConfig.noLoadCurrent = doc["noLoadCurrent"].as<float>();
  }
  if (doc.containsKey("loadCurrent"))
  {
    smartConfig.loadCurrent = doc["loadCurrent"].as<float>();
  }
  if (doc.containsKey("testRunTime"))
  {
    smartConfig.testRunTime = doc["testRunTime"].as<unsigned long>();
  }
  if (doc.containsKey("waitTime"))
  {
    smartConfig.waitTime = doc["waitTime"].as<unsigned long>();
  }
  if (doc.containsKey("maxTestTime"))
  {
    unsigned long newMaxTestTime = doc["maxTestTime"].as<unsigned long>();
    if (newMaxTestTime < smartConfig.testRunTime + smartConfig.waitTime)
    {
      server.send(400, "text/plain", "Max test time must be greater than test run time + wait time");
      return;
    }
    smartConfig.maxTestTime = newMaxTestTime;
  }

  server.send(200, "text/plain", "Smart configuration updated");
}

void handleSensorData()
{
  readCurrent();
  String json = "{\"current\":" + String(currentValue, 2) +
                ",\"power\":" + String(powerWatts, 2) +
                ",\"pumpOn\":" + (pumpOn ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleStatus()
{
  time_t now;
  time(&now);

  // Build smart status JSON
  String smartStatusJson = "\"smartStatus\":{";
  smartStatusJson += "\"state\":\"";
  switch (smartStatus.state)
  {
  case IDLE:
    smartStatusJson += "IDLE";
    break;
  case NORMAL_RUN:
    smartStatusJson += "NORMAL_RUN";
    break;
  case TEST_RUN:
    smartStatusJson += "TEST_RUN";
    break;
  case WAITING:
    smartStatusJson += "WAITING";
    break;
  case COMPLETED:
    smartStatusJson += "COMPLETED";
    break;
  }
  smartStatusJson += "\",";
  smartStatusJson += "\"waterPresent\":" + String(smartStatus.waterPresent ? "true" : "false") + ",";
  smartStatusJson += "\"testAttempts\":" + String(smartStatus.testAttempts) + ",";
  smartStatusJson += "\"totalRunTimeWithLoad\":" + String(smartStatus.totalRunTimeWithLoad) + ",";
  smartStatusJson += "\"totalRunTimeNoLoad\":" + String(smartStatus.totalRunTimeNoLoad) + ",";
  smartStatusJson += "\"lastLogMessage\":\"" + smartStatus.lastLogMessage + "\"";
  smartStatusJson += "}";

  String json = "{\"pumpOn\":" + String(pumpOn ? "true" : "false") +
                ",\"scheduleEnabled\":" + String(scheduleEnabled ? "true" : "false") +
                ",\"currentTime\":" + String(now) + "," + smartStatusJson + "}";
  server.send(200, "application/json", json);
}

void setup()
{
  Serial.begin(115200);

  // Initialize pins
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(currentSensorPin, INPUT);

  Serial.println("Starting Smart ESP32 Pump Controller...");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  // Configure NTP for real-time clock
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); // GMT+5:30 for India
  Serial.println("Waiting for time synchronization...");

  struct tm timeinfo;
  int timeoutCount = 0;
  while (!getLocalTime(&timeinfo) && timeoutCount < 10)
  {
    delay(1000);
    timeoutCount++;
    Serial.print(".");
  }

  if (timeoutCount < 10)
  {
    Serial.println();
    Serial.println("Time synchronized");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
  else
  {
    Serial.println();
    Serial.println("Time sync failed - continuing without NTP");
  }

  // Setup OTA
  ArduinoOTA.setHostname("ESP32-SmartPump");
  ArduinoOTA.onStart([]()
                     {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type); });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed"); });
  ArduinoOTA.begin();

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/schedule", HTTP_POST, handleSchedule); // Keep old endpoint for compatibility
  server.on("/smart-schedule", HTTP_POST, handleSmartSchedule);
  server.on("/smart-config", HTTP_POST, handleSmartConfig);
  server.on("/sensor", HTTP_GET, handleSensorData);
  server.on("/status", HTTP_GET, handleStatus);

  server.begin();
  Serial.println("HTTP server started with Smart Pump Intelligence");

  logPumpActivity("Smart pump controller initialized");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  // Handle smart pump logic first
  updateSmartPump();

  // Handle traditional scheduling only when smart pump is idle
  if (scheduleEnabled && smartStatus.state == IDLE) {
    time_t now;
    time(&now);

    if (useDuration) {
      // Simple duration-based scheduling
      if (durationStartTime == 0) {
        durationStartTime = now;
        togglePump(true);
        Serial.println("Duration schedule started");
      } else if (now - durationStartTime >= durationSeconds) {
        togglePump(false);
        scheduleEnabled = false;
        durationStartTime = 0;
        Serial.println("Duration schedule completed");
      } 
    } else {
      // Time-based scheduling - fixed logic
      if (now >= scheduleStart && now < scheduleEnd) {
        if (!pumpOn) {
          togglePump(true);
          Serial.println("Time schedule started");
        }
      } else if (now >= scheduleEnd) {
        if (pumpOn) {
          togglePump(false);
        }
        scheduleEnabled = false;
        Serial.println("Time schedule completed");
      }
    }
  }

  delay(100);
}
