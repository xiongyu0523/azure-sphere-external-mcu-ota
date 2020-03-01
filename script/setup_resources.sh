#!/bin/bash

# On Git for Windows
if [[ "$OSTYPE" == "msys" ]]; then
    az() {
        az.cmd $1 $2 $3 $4 $5 $6 $7 $8 $9 ${10} ${11} ${12} ${13} ${14} ${15} ${16}
    }
fi

# Credentials and tenant, user should set env variables APP_ID, PASSWORD, TENANT_ID outside of script

REGION="eastasia"
RESOURCE_GROUP="extmcuota-group"
IOTHUB="extmcuota-hub"
DPS="extmcuota-dps"
STORAGE_ACCOUNT="extmcuotastorage"
CONTAINER="ota"

# Login with Service Principal
az login --service-principal --username $APP_ID --password $PASSWORD --tenant $TENANT_ID

# Create a resource group.
az group create --name $RESOURCE_GROUP --location $REGION

# Create a IoT Hub
az iot hub create --name $IOTHUB --resource-group $RESOURCE_GROUP --location $REGION --sku F1

# Create a IoT Hub DPS
az iot dps create --name $DPS --resource-group $RESOURCE_GROUP --location $REGION

# Get conection string
hubConnectionString=$(az iot hub show-connection-string --name $IOTHUB --key primary --policy-name iothubowner --query connectionString -o tsv)

# Link IoT Hub and DPS
az iot dps linked-hub create --dps-name $DPS --resource-group $RESOURCE_GROUP --connection-string $hubConnectionString --location $REGION

# download tenant CA certificate, user must use azsphere login command to login Azure Sphere Security Service and select a tenant before running this command
azsphere tenant download-CA-certificate --output CAcertificate.cer

# Upload tenant CA certificate
etag=$(az iot dps certificate create --dps-name $DPS --resource-group $RESOURCE_GROUP --name TenantCACertificate --path ./CACertificate.cer --query etag -o tsv)

# Generate verification code
ret=`az iot dps certificate generate-verification-code --dps-name $DPS --resource-group $RESOURCE_GROUP --name TenantCACertificate --etag $etag --query [properties.verificationCode,etag] -o tsv`
{ read verifyCode; read etag; } <<< "$ret"

# request AS3 to sign verficaiton certificate
azsphere tenant download-validation-certificate --output ValidationCertification.cer --verificationcode $verifyCode

# Proof-of-possesion 
az iot dps certificate verify --dps-name $DPS --resource-group $RESOURCE_GROUP --name TenantCACertificate --path ./ValidationCertification.cer --etag $etag

# create storage account for OTA file
az storage account create --name $STORAGE_ACCOUNT --location $REGION --resource-group $RESOURCE_GROUP --kind StorageV2

# Get Azure Storage account connection string
connectionStr=$(az storage account show-connection-string --resource-group $RESOURCE_GROUP --name $STORAGE_ACCOUNT --query connectionString -o tsv)

# create a container
az storage container create --name $CONTAINER --public-access off --connection-string $connectionStr

# clean & logout
rm CACertificate.cer
rm ValidationCertification.cer
az logout
