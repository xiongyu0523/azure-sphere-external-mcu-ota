#!/bin/bash

# On Git for Windows
if [[ "$OSTYPE" == "msys" ]]; then
    az() {
        az.cmd $1 $2 $3 $4 $5 $6 $7 $8 $9 ${10} ${11} ${12} ${13} ${14} ${15} ${16}
    }
fi

# !!!!!!!!! BE CAREFUL !!!!!!!!!, delete action will remove everything under a resource group
RESOURCE_GROUP="azsphere-extmcuota-group"

# Login with Service Principal
az login --service-principal --username $APP_ID --password $PASSWORD --tenant $TENANT_ID

# Delete resource group

az group delete --name $RESOURCE_GROUP --yes

# logout
az logout
