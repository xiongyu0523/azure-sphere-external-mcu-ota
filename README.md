# Azure Sphere External MCU OTA reference design

This reference prject is to demostrate how user can upgrade an external MCU connected to Azure Sphere MT3620 using built-in automatic device management feature of Azure IoT Hub and Azure Blob storage. 

Though official repo has a [ExternalMcuUpdate](https://github.com/Azure/azure-sphere-samples/tree/master/Samples/ExternalMcuUpdate) sample project showing how to update a nRF52 BLE device connected to Azure Sphere, that example has embedded the image of nRF52 BLE as part of the MT3620's image. Hence the allowed image size of external is limited to free flash space Azure Sphere has (up to 1MB). This reference project try to solve another real use case when user need to deal with a connected external MCU has a much bigger firmware size say a few Mega bytes, it provide a more generic implementation and framework for user to continue customize and improve.  

This project includes
  - Azure Sphere MT3620 Firmware
  - A bash script [setup_resources.sh](./script/setup_resources.sh) to provided to ease all required azure resources provisioning. 
  - A bash script [clean_resources.sh](./script/clean_resources.sh) 
  - A python script [ota.py](./script/ota.py) to provided to deploy a OTA update. 

## Design considerations

1. There is no real mcu device need to be connected to MT3620 to work with the demo. Since different MCU has different protocol to upgrade its firmware, this part should be customized by user who actually selects the MCU. Instead, the demo will download the external MCU firmware image into an SPI flash to generialize the implementation. Once a firmware image is donwloaded and passed integrty check, user can easily program external MCU accroding to the spec of target MCU. (E.g. MCUboot protocol for NXP MCUs)

2. Reslience is the most basic requirement for OTA. Since end device may suffer network interrupt or power-fail at any time during operation, the device must be prepared and robust enough for these failures for OTA as well. Thanks to built-in libcurl service, resume download has been implemeneted easily with this reference. To understand where is the proper recovery point, we need poll some information from non-volatile memory, a littlefs file system is mounted on external spi flash for image backup and this record informaiton, this tiny file system for embeddded system is wellkown by its fail-safe capaiblity.

## To build and run the sample

### Prerequisite

1. Visual Studio 2019
2. Azure Sphere SDK 20.01 for Visual Studio
3. Python 3.x
4. Git
5. Azure CLI with Azure IoT Hub extension
6. Azure IoT SDK for Python V2
7. Azure subscription
8. Microsoft Account

### Prep your device and environment

1. Ensure that your Azure Sphere MT3620 is connected to your PC, and your PC is connected to the internet.

2. Login Azure Sphere Security Service using your microsoft account registered before and select proper tenant for your device, or claim the device into a tenant for the first time using the device. Check this [page](https://docs.microsoft.com/en-us/azure-sphere/install/claim-device) for details.
   
3. Right-click the Azure Sphere Developer Command Prompt shortcut and issue the following command to ensure your device has development capability:

   ```
   azsphere dev edv
   ```

4. Hardware conection (Tested with W25Q128JV SPI Flash from Winbond)
   
    |  SPI FLASH | RDB  | MT3620 |
    |  ----  | ----  | ---- | 
    | MOSI  | H2-7 | GPIO27_MOSI0_RTS0_CLK0 |
    | MISO  | H2-1 | GPIO28_MISO0_RXD0_DATA0 |
    | CLK | H2-3 | GPIO26_SCLK0_TXD0 |
    | CS  | H1-4 | GPIO5 |
    | VCC  | H3-3 | 3V3 | 
    | GND  | H3-2 | GND |

### Provision Azure resources

Several Azure resources need to be provisioned before running this project. They are:

- A resource group to hold all Azure resources below
- A Azure IoT Hub
- A Azure IoT Hub DPS 
- A Azure Stroage account
- A Azure Stroage container

A bash script [setup_resrouces.sh](./script/setup_resources.sh) can be find under script folder for this purpose. Before excecute the script, you need export three environment variables APP_ID, PASSWORD and TENANT_ID as service princiapl and tenant information to authenticate within your script. Check this [page](https://docs.microsoft.com/en-us/cli/azure/create-an-azure-service-principal-azure-cli?view=azure-cli-latest) for the details of how to create a service principal in Azure CLI.

### Build and run Firmware

1. Clone this project using `git clone https://github.com/xiongyu0523/azure-sphere-external-mcu-ota ----recurse-submodules`
2. Start Visual Studio.
3. From the **File** menu, select **Open > CMake...** and navigate to the folder that contains the sample to load
4. Open app_manifest.json file to fill neccessary claim for your application:
   - "CmdArgs": [ "scope Id in your DPS" ]
   - "AllowedConnections": [ "global.azure-devices-provisioning.net", "your-iot-hub.azure-devices.net", "yourstroageaccount.blob.core.windows.net" ]
   - "DeviceAuthentication": "Azure Sphere tenant Id"
5. Select the file CMakeLists.txt and then click **Open**. 
6. In Solution Explorer, right-click the CMakeLists.txt file, and select **Generate Cache for azure-sphere-extmcu-ota**. This step performs the cmake build process to generate the native ninja build files. 
7. In Solution Explorer, right-click the *CMakeLists.txt* file, and select **Build** to build the project and generate .imagepackage target.
8. Double click *CMakeLists.txt* file and press F5 to start the application with debugging. 
9. You will start to see Azure Sphere is connected to IoT Hub and start to send telemetry data to your IoT Hub. 

### Deploy a OTA

This project leverage the IoT device management feature in Azure IoT Hub to automatic complex tasks of managing large device fleets and Azure blob stroage is hold the firmware images and support for downloading at scale. 

A python script [ota.py](./script/ota.py) is provided for deploying a new firmware. The minimial positional paramters are full path of the image, version of the image, targeted product type and device group for this deployment.

```
usage: ota.py [-h] [-c CONTAINER] [-d DAYS] FILE VERSION PRODUCT GROUP

positional arguments:
  FILE                  Full path of file for ota
  VERSION               Version of the file, must > 0
  PRODUCT               Target product type
  GROUP                 Target group under a product

optional arguments:
  -h, --help            show this help message and exit
  -c CONTAINER, --container CONTAINER
                        specify the container of blob
  -d DAYS, --days DAYS  sas expire duration
```

Below example deploys a new firmware update target washingmachie2020 devices in field_test group

```
python ota.py c:/mcu.bin 5 washingmachie2020 field_test
```

> For simplicity, initial firmware version is considerated always start from 0 and increase afterwards, version roll back is not allowed. 

### Cleanup resources

Run [clean_resources.sh](./scripts/clean_resources.sh) script to clean everything provisioned on Azure within this demo. 

> BE CAREFUL! This script will delete all services under the resource group, and CAN NOT be restored. Please make sure you have selected the correct resource group within the script. 

## OSS and License

This project has submoduled several open sources project on github, please refer to their license for details. 

- [littlefs](https://github.com/ARMmbed/littlefs)
- [spiflash_driver](https://github.com/pellepl/spiflash_driver)
- [SHA256](https://github.com/ilvn/SHA256)
- [azure-sphere-samples](https://github.com/Azure/azure-sphere-samples)

All other code developed are 

MIT License

Copyright (c) 2020 neo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
