import os
import sys
import re
import argparse
import json
import hashlib
from datetime import datetime, timedelta
from azure.iot.hub import IoTHubRegistryManager
from azure.iot.hub import IoTHubConfigurationManager
from azure.iot.hub import models
from azure.storage.blob import BlobServiceClient, generate_container_sas, ContainerSasPermissions

blob_conn_str = os.environ["AZURE_STORAGE_CONNECTIONSTRING"]
stroage_account_name = re.search('AccountName=(.*);AccountKey=', blob_conn_str).group(1)
account_access_key = re.search('AccountKey=(.*);EndpointSuffix=', blob_conn_str).group(1)

def upload_file(file, container):

    file_name = os.path.basename(file)
    blob_service_client = BlobServiceClient.from_connection_string(conn_str=blob_conn_str)

    blob_client = blob_service_client.get_blob_client(container=container, blob=file_name)
    with open(file, "rb") as data:
        blob_client.upload_blob(data, overwrite=True)

def deploy(file, version, product, group, container, days):

    file_size = os.stat(file).st_size
    file_url  = f"https://{stroage_account_name}.blob.core.windows.net/{container}/{os.path.basename(file)}"
    file_sas  = generate_container_sas(
        account_name=stroage_account_name,
        container_name=container,
        account_key=account_access_key,
        permission=ContainerSasPermissions(read=True, list=True),
        expiry=datetime.utcnow() + timedelta(days=days)
    )

    with open(file, "rb") as f:
        file_sha256 = hashlib.sha256(f.read()).hexdigest().upper()

    iothub_conn_str = os.environ["AZURE_IOTHUB_CONNECTIONSTRING"]
    iothub_configuration = IoTHubConfigurationManager(iothub_conn_str)

    config = models.Configuration()

    config.id = "ota_v" + str(version)
    config.content = models.ConfigurationContent(device_content={
        "properties.desired.extFwInfo":{
            "version" : version,
            "size" : file_size,
            "url" : file_url,
            "sas" : file_sas,
            "sha256" : file_sha256
        }
    })

    config.metrics = models.ConfigurationMetrics(queries={
            "Downloading": f"SELECT deviceId FROM devices WHERE configurations.[[{config.id}]].status='Applied' AND properties.reported.extFwInfo.Status='downloading'",
            "Interrupted": f"SELECT deviceId FROM devices WHERE configurations.[[{config.id}]].status='Applied' AND properties.reported.extFwInfo.Status='interrupted'",
            "Applying": f"SELECT deviceId FROM devices WHERE configurations.[[{config.id}]].status='Applied' AND properties.reported.extFwInfo.Status='applying'",
            "Applied": f"SELECT deviceId FROM devices WHERE configurations.[[{config.id}]].status='Applied' AND properties.reported.extFwInfo.Status='applied'",
            "Error": f"SELECT deviceId FROM devices WHERE configurations.[[{config.id}]].status='Applied' AND properties.reported.extFwInfo.Status='error'"          
        }
    )

    config.target_condition = f"tags.productType='{product}' AND tags.deviceGroup='{group}'"
    config.priority = version

    iothub_configuration.create_configuration(config)

if __name__ == "__main__":

    # Let's deal with arguments
    parser = argparse.ArgumentParser()
    parser.add_argument("FILE", type=str, help="Full path of file for ota")
    parser.add_argument("VERSION", type=int, help="Version of the file, must > 0")
    parser.add_argument("PRODUCT", type=str, help="Target product type")
    parser.add_argument("GROUP", type=str, help="Target group under a product")
    parser.add_argument("-c", "--container", type=str, default="ota", help="specify the container of blob")
    parser.add_argument("-d", "--days", type=int, default=365, help="sas expire duration")
    args = parser.parse_args()

    if args.VERSION <= 0:
        raise ValueError("version should > 0")

    # Step1: upload the file to azure blob
    upload_file(args.FILE, args.container)
    # Step2: create a IoT device configuration
    deploy(args.FILE, args.VERSION, args.PRODUCT, args.GROUP, args.container, args.days)






    
