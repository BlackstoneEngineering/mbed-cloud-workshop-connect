// ----------------------------------------------------------------------------
// Copyright 2016-2018 ARM Ltd.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------------------------------------------------------

#include "mbed.h"
#include <stdio.h>
#include <errno.h>

#include "qspi_api.h"
#include "QSPI.h"
#include "qspi-blockdevice/QSPIFBlockDevice.h"
#define DEVICE_QSPI

#include "FATFileSystem.h"
#include "LittleFileSystem.h"
#include "simple-mbed-cloud-client.h"
// #include "SDBlockDevice.h"
#include "ISM43362Interface.h"
// #include "SPIFBlockDevice.h" // to use onboard spi flash

#define WIFI_SSID       "mbed"
#define WIFI_PASSWORD   "workshop-password"




// An event queue is a very useful structure to debounce information between contexts (e.g. ISR and normal threads)
// This is great because things such as network operations are illegal in ISR, so updating a resource in a button's fall() function is not allowed
EventQueue eventQueue;

// Declaring net interface as a global variable instead of local to avoid stack overflow
ISM43362Interface net;

// Storage implementation definition, currently using SDBlockDevice (SPI flash, DataFlash, and internal flash are also available)
// SDBlockDevice sd(SPI_MOSI, SPI_MISO, SPI_SCK, SPI_CS);
QSPIFBlockDevice sd(PE_12, PE_13, PE_14, PE_15,PE_10,PE_11,0,8000000);

// FATFileSystem fs("sd", &bd);

LittleFileSystem fs("sd");

// Declaring pointers for access to Mbed Cloud Client resources outside of main()
MbedCloudClientResource *button_res;
MbedCloudClientResource *pattern_res;

// This function gets triggered by the timer. It's easy to replace it by an InterruptIn and fall() mode on a real button
void fake_button_press() {
    int v = button_res->get_value_int() + 1;

    button_res->set_value(v);

    printf("Simulated button clicked %d times\n", v);
}

/**
 * PUT handler
 * @param resource The resource that triggered the callback
 * @param newValue Updated value for the resource
 */
void pattern_updated(MbedCloudClientResource *resource, m2m::String newValue) {
    printf("PUT received, new value: %s\n", newValue.c_str());
}

/**
 * POST handler
 * @param resource The resource that triggered the callback
 * @param buffer If a body was passed to the POST function, this contains the data.
 *               Note that the buffer is deallocated after leaving this function, so copy it if you need it longer.
 * @param size Size of the body
 */
void blink_callback(MbedCloudClientResource *resource, const uint8_t *buffer, uint16_t size) {
    printf("POST received. Going to blink LED pattern: %s\n", pattern_res->get_value().c_str());

    static DigitalOut augmentedLed(LED1); // LED that is used for blinking the pattern

    // Parse the pattern string, and toggle the LED in that pattern
    string s = std::string(pattern_res->get_value().c_str());
    size_t i = 0;
    size_t pos = s.find(':');
    while (pos != string::npos) {
        wait_ms(atoi(s.substr(i, pos - i).c_str()));
        augmentedLed = !augmentedLed;

        i = ++pos;
        pos = s.find(':', pos);

        if (pos == string::npos) {
            wait_ms(atoi(s.substr(i, s.length()).c_str()));
            augmentedLed = !augmentedLed;
        }
    }
}

/**
 * Notification callback handler
 * @param resource The resource that triggered the callback
 * @param status The delivery status of the notification
 */
void button_callback(MbedCloudClientResource *resource, const NoticationDeliveryStatus status) {
    printf("Button notification, status %s (%d)\n", MbedCloudClientResource::delivery_status_to_string(status), status);
}

/**
 * Registration callback handler
 * @param endpoint Information about the registered endpoint such as the name (so you can find it back in portal)
 */
void registered(const ConnectorClientEndpointInfo *endpoint) {
    printf("Connected to Mbed Cloud. Endpoint Name: %s\n", endpoint->internal_endpoint_name.c_str());
}

int main(void) {
    printf("Starting Simple Mbed Cloud Client example\n");

    printf("Checking SDCard is Formatted\r\n");
    int err = fs.mount(&sd);
    printf("%s\n", (err ? "Fail :(" : "OK"));
    if (err) {
        // Reformat if we can't mount the filesystem
        // this should only happen on the first boot
        printf("No filesystem found, formatting... ");
        fflush(stdout);
        err = fs.reformat(&sd);
        printf("%s\n", (err ? "Fail :(" : "OK"));
        if (err) {
            error("error: %s (%d)\n", strerror(-err), err);
        }
    }
    // err = fs.unmount();
    // printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
    // if (err < 0) {
    //     error("error: %s (%d)\n", strerror(-err), err);
    // }

    printf("Connecting to the network using Wifi...\n");

    // Connect to the internet (DHCP is expected to be on)
    nsapi_error_t status = net.connect(WIFI_SSID, WIFI_PASSWORD, (strlen(WIFI_PASSWORD) > 1) ? NSAPI_SECURITY_WPA_WPA2 : NSAPI_SECURITY_NONE);

    if (status != 0) {
        printf("Connecting to the network failed %d!\n", status);
        return -1;
    }

    printf("Connected to the network successfully. IP address: %s\n", net.get_ip_address());

    // SimpleMbedCloudClient handles registering over LwM2M to Mbed Cloud
    SimpleMbedCloudClient client(&net, &sd, &fs);
    int client_status = client.init();
    if (client_status != 0) {
        printf("Initializing Mbed Cloud Client failed (%d)\n", client_status);
        return -1;
    }

    // Creating resources, which can be written or read from the cloud
    button_res = client.create_resource("3200/0/5501", "button_count");
    button_res->set_value(0);
    button_res->methods(M2MMethod::GET);
    button_res->observable(true);
    button_res->attach_notification_callback(button_callback);

    pattern_res = client.create_resource("3201/0/5853", "blink_pattern");
    pattern_res->set_value("500:500:500:500:500:500:500:500");
    pattern_res->methods(M2MMethod::GET | M2MMethod::PUT);
    pattern_res->attach_put_callback(pattern_updated);

    MbedCloudClientResource *blink_res = client.create_resource("3201/0/5850", "blink_action");
    blink_res->methods(M2MMethod::POST);
    blink_res->attach_post_callback(blink_callback);

    printf("Initialized Mbed Cloud Client. Registering...\n");

    // Callback that fires when registering is complete
    client.on_registered(&registered);

    // Register with Mbed Cloud
    client.register_and_connect();

    // Placeholder for callback to update local resource when GET comes.
    // The timer fires on an interrupt context, but debounces it to the eventqueue, so it's safe to do network operations
    Ticker timer;
    timer.attach(eventQueue.event(&fake_button_press), 5.0);

    // You can easily run the eventQueue in a separate thread if required
    eventQueue.dispatch_forever();
}
