# Azure Sphere External MCU OTA reference design

This reference design is to demostrate how user can upgrade an external MCU connected to Azure Sphere device using Azure IoT Hub automatic device management and Azure Blob storage service. 

Though official repo has a [demo]() project showing how to update a nRF52 BLE device connected to Azure Sphere, that example has embedded the image of nRF52 BLE as part of Azure Sphere's image, hence the allowed image size of external is limited to free flash space Azure Sphere has (e.g. 256KB). A more real use case is user  need to deal with a external MCU has a much bigger firmware size (few MB), this reference design provide a more generic implementation using Azure IoT Hub and Azure blob storage service. 

This reference design include 
  - Azure Sphere Firmware
  - A bash script [setup_resources.sh](./script/setup_resources.sh) to provided to ease all required azure resources provisioning. 
  - A bash script [clean_resources.sh](./script/clean_resources.sh) 
  - A python script [ota.py](./script/ota.py) to provided to deploy a OTA update. 

## Design ideas

1. There is no real mcu device need to be connected to Azure Sphere to work with demo. Since different MCU has different protocol to upgrade its firmware. That part should be customized by end user who actually selects the MCU. Instead, the demo will download the firmware image file to an external SPI memory to generialize the implementation. Once a firmware image is donwloaded into local memory and pass its integrty check, user can easily program external mcu using Azure Sphere. 

2. Reslience is the most basic requirement for OTA. Since end device will suffer network interrupt or power fail at any time, the device must be prepared and robust enough for these failures. Thanks to built-in libcurl library, resume download can be impelmented easily with this reference. A littlefs file system is mounted on external spi flash for image backup, this tiny file system is wellkown by its fail-safe capability.

## To build and run the sample

### Prerequisite

1. Visual Studio 2019
2. Azure Sphere SDK 20.01 for Visual Studio
3. Python 3.x
4. Git
5. Azure CLI with Azure IoT Hub extension
6. Azure IoT SDK for Python V2
7. Azure subscription 

### Prep your device and environment

1. Ensure that your Azure Sphere device is connected to your PC, and your PC is connected to the internet.

2. Login Azure Sphere Security Service using your credential and select tenant for your device, or claim the device into a tenant for the first time using the device. Check this [page](https://docs.microsoft.com/en-us/azure-sphere/install/claim-device) for details.
   
3. Right-click the Azure Sphere Developer Command Prompt shortcut and issue the following command to ensure your device has development capability:

   ```
   azsphere dev edv
   ```

4. Hardware conection:
   
    |  SPI FLASH | RDB  | MT3620 |
    |  ----  | ----  | ---- | 
    | MOSI  | H2-7 | GPIO27_MOSI0_RTS0_CLK0 |
    | MISO  | H2-1 | GPIO28_MISO0_RXD0_DATA0 |
    | CLK | H2-3 | GPIO26_SCLK0_TXD0 |
    | CS  | H1-4 | GPIO5 |
    | VCC  | H3-3 | 3V3 | 
    | GND  | H3-2 | GND |

### Provision Azure resources

Serveral azure sources need to be provisioned before running this demo. They are:

- A resource group to hold all other azure resources
- A Azure IoT Hub
- A Azure IoT Hub DPS 
- A Azure Stroage account
- A Azure Stroage container

A bash script [setup_resrouces.sh](./script/setup_resources.sh) can be find under script folder for this purpose. Before excecute the script, you need export three environment variables APP_ID, PASSWORD TENANT_ID as service princiapl and tenant information to authenticate within your script. Check this [page](https://docs.microsoft.com/en-us/cli/azure/create-an-azure-service-principal-azure-cli?view=azure-cli-latest) for the details of how to create a service principal in Azure cli

### Build and run Firmware

1. Start Visual Studio.
2. From the **File** menu, select **Open > CMake...** and navigate to the folder that contains the sample.
3. Select the file CMakeLists.txt and then click **Open**. 
4. In Solution Explorer, right-click the CMakeLists.txt file, and select **Generate Cache for azure-sphere-extmcu-ota**. This step performs the cmake build process to generate the native ninja build files. 
5. In Solution Explorer, right-click the *CMakeLists.txt* file, and select **Build** to build the project and generate .imagepackage target.
6. Double click *CMakeLists.txt* file and press F5 to start the application with debugging. 
7. You will start to see Azure Sphere is connected to IoT Hub and start to send telemetry data to your IoT Hub. 

### Deploy a OTA

To deploy a new firmware for external MCU, the design levearge the automatic device management feature in Azure IoT Hub to automatic complex tasks of managing large device fleets. Also a Azure blob stroage is used to hold the actual firmware and support for download at scale. 

A python script [ota.py](./script/ota.py) is provided for deploying a new firmware. The minimial paramters are full path of the image, version of the image, targeted product type and device group for this deployment. 

Below example will deploy a new firmware update target washingmachie2020 devices in field_test group

```
python ota.py c:/mcu.bin 5 washingmachie2020 field_test
```

> For simplicity, firmware version is considerated always start from 0 and roll back is not allowed

### Cleanup resources

Run [clean_resources.sh](./scripts/clean_resources.sh) script to clean everything provisioned on Azure within this demo. 

> BE CAREFUL! This script will delete all services under the resource group, and CAN NOT be restored. Please make sure you have selected the correct resource group within the script. 
