#!/bin/bash

# On Git for Windows
if [[ "$OSTYPE" == "msys" ]]; then
    az() {
        az.cmd $1 $2 $3 $4 $5 $6 $7 $8 $9 ${10} ${11} ${12} ${13} ${14} ${15} ${16}
    }
fi

# Credentials and tenant
APP_ID="dc86a62e-1306-460b-a834-7388689c68f7"
PASSWORD="e790d247-2c49-46a2-8497-c53ad24b7fe2"
TENANT_ID="72f988bf-86f1-41af-91ab-2d7cd011db47"

# Login with Service Principal
az login --service-principal --username $APP_ID --password $PASSWORD --tenant $TENANT_ID

# Create a resource group.
az group create --name myTestResourceGroup --location eastasia


