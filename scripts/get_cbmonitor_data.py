'''
Copyright 2018 Couchbase, Inc

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

The script downloads the raw cbmonitor data for a given list of runs and dumps
some performance metrics to file (JSON and CSV formats).

Usage: python get_cbmonitor_data.py --job_list \
       <project1>:<number1>[:'<label1>'] [<project2>:<number2> ..] \
       --output_dir <output_dir>
(e.g., python get_cbmonitor_data.py --job_list \
       hera-pl:60:'RocksDB low OPS' hera-pl:67 --output_dir . )
'''

import argparse
import csv
import json
import re
import sys
import urllib2


# data format: [[timestamp1,value1],[timestamp2,value2],..]
def downloadData(url):
    print("downloading: " + url)
    try:
        # Note: urllib2 module sends HTTP/1.1 requests with Connection:close
        # header included
        connection = urllib2.urlopen(url, timeout=10)
    except urllib2.URLError, e:
        raise Exception("'urlopen' error, url not correct or user not "
                        "connected to VPN: %r" % e)
    return connection.read()


def getAverage(url):
    accumulator = 0.0
    pairs = json.loads(downloadData(url))
    for pair in pairs:
        accumulator += float(pair[1])
    return accumulator/len(pairs)


def getAverageFromList(urls, per_single_node):
    accumulator = 0.0
    for url in urls:
        accumulator += getAverage(url)
    return (accumulator/len(urls) if per_single_node else accumulator)


def getMax(url):
    maximum = 0.0
    pairs = json.loads(downloadData(url))
    for pair in pairs:
        value = float(pair[1])
        if value > maximum:
            maximum = value
    return maximum


usage = ("Usage: python get_cbmonitor_data.py --job_list "
         "<project1>:<number1>[:'<label1>'] [<project2>:<number2> ..] "
         "--output_dir <output_dir> "
         "\n\t(e.g., python get_cbmonitor_data.py --job_list "
         "hera-pl:60:'RocksDB low OPS' hera-pl:67 hera-pl:83 --output_dir . )")

ap = argparse.ArgumentParser()

try:
    ap.add_argument('--job_list', nargs='+')
    ap.add_argument('--output_dir')
except:
    print(usage)
    sys.exit()

args, leftovers = ap.parse_known_args()

if (args.job_list is None) or (args.output_dir is None):
    print(usage)
    sys.exit()

job_list = args.job_list
output_dir = args.output_dir

print("Job list: " + str(job_list))
print("Output dir: " + output_dir)


byteToMBConversionFactor = 1.0/(1024*1024)
host = "http://cbmonitor.sc.couchbase.com:8080"

# Keep data for all jobs to build an aggregated CSV file
data_list = []
# Main loop
for job in job_list:
    print("**************************************")
    print("Job: " + job)
    print("**************************************")

    array = job.split(":")
    if len(array) < 2:
        print(usage)
        sys.exit()
    project = array[0]
    number = array[1]
    label = array[2] if (len(array) == 3) else ""

    consoleText = downloadData("http://perf.jenkins.couchbase.com/job/" +
                               project + "/" + number + "/consoleText")

    if consoleText.find("snapshot=") == -1:
        print("Snapshot not found for job" + job + ", probably the job has "
              "been aborted. Skipping..")
        continue
    snapshot = re.search("snapshot=.*", consoleText).group(0).split("=")[1]
    print("snapshot: " + snapshot)

    '''
    Regex to get nodes IP from e.g.:
        2018-01-16T11:35:11 [INFO] Getting memcached port from 172.23.96.100
        2018-01-16T11:35:11 [INFO] Getting memcached port from 172.23.96.101
        2018-01-16T11:35:11 [INFO] Getting memcached port from 172.23.96.102
        2018-01-16T11:35:11 [INFO] Getting memcached port from 172.23.96.103
    '''
    memcachedMatchAll = re.findall("Getting memcached port from.*",
                                   consoleText)
    nodes = []
    for match in memcachedMatchAll:
        nodes.append(match.split("from")[1].strip().replace(".", ""))
    nodes = set(nodes)
    print("nodes: " + str(nodes))

    '''
    Regex to get bucket name from e.g.:
        2018-01-16T09:19:17 [INFO] Adding new bucket: bucket-1
    '''
    bucket = re.search("Adding new bucket.*", consoleText) \
        .group(0) \
        .split(":")[1] \
        .strip()
    print("bucket: " + bucket)

    # url format: [host + "/" + "<dataset>" + snapshot [+ "<ip/bucket>"] +
    #              "/" + "<resource>"]
    base_url_ns_server = host + "/" + "ns_server" + snapshot + bucket
    base_url_spring_latency = host + "/" + "spring_latency" + snapshot + bucket

    # ops
    ops = host + "/" + "ns_server" + snapshot + "/ops"
    # latency
    latency_get = base_url_spring_latency + "/latency_get"
    latency_set = base_url_spring_latency + "/latency_set"
    # other interesting timings
    avg_bg_wait_time = base_url_ns_server + "/avg_bg_wait_time"
    avg_disk_commit_time = base_url_ns_server + "/avg_disk_commit_time"
    avg_disk_update_time = base_url_ns_server + "/avg_disk_update_time"
    # read/write amplification
    data_rps = []
    data_wps = []
    data_rbps = []
    data_wbps = []
    for node in nodes:
        base_url_iostat = host + "/" + "iostat" + snapshot + node
        data_rps.append(base_url_iostat + "/data_rps")
        data_wps.append(base_url_iostat + "/data_wps")
        data_rbps.append(base_url_iostat + "/data_rbps")
        data_wbps.append(base_url_iostat + "/data_wbps")
    # disk amplification
    couch_total_disk_size = base_url_ns_server + "/couch_total_disk_size"
    # memory usage
    base_url_atop = host + "/" + "atop" + snapshot
    memcached_rss = []
    for node in nodes:
        memcached_rss.append(base_url_atop + node + "/memcached_rss")
    mem_used = base_url_ns_server + "/mem_used"
    # cpu usage
    memcached_cpu = []
    for node in nodes:
        memcached_cpu.append(base_url_atop + node + "/memcached_cpu")

    # Perfrunner collects the 'spring_latency' dataset for 'latency_set' and
    # 'latency_get' only on some test configs (i.e., synchronous clients)
    hasLatency = True if (consoleText.find("spring_latency") != -1) else False

    data = {
        'job': project + ":" + number,
        'label': label,
        'snapshot': snapshot,
        'ops': '{:.2f}'.format(getAverage(ops)),
        'latency_set': '{:.2f}'.format(getAverage(latency_set))
                       if hasLatency else 'N/A',
        'latency_get': '{:.2f}'.format(getAverage(latency_get))
                       if hasLatency else 'N/A',
        'avg_disk_update_time (us)': '{:.2f}'
                                     .format(getAverage(avg_disk_update_time)),
        'avg_disk_commit_time (s)': '{:.2f}'
                                     .format(getAverage(avg_disk_commit_time)),
        'avg_bg_wait_time (us)': '{:.2f}'.format(getAverage(avg_bg_wait_time)),
        'data_rps': '{:.2f}'.format(getAverageFromList(data_rps, True)),
        'data_wps': '{:.2f}'.format(getAverageFromList(data_wps, True)),
        'data_rbps (MB/s)': '{:.2f}'.format(
                                        getAverageFromList(data_rbps, True) *
                                        byteToMBConversionFactor
                                     ),
        'data_wbps (MB/s)': '{:.2f}'.format(
                                        getAverageFromList(data_wbps, True) *
                                        byteToMBConversionFactor
                                    ),
        'couch_total_disk_size (MB)': '{:.2f}'
                                      .format(
                                          getAverage(couch_total_disk_size) *
                                          byteToMBConversionFactor
                                      ),
        'max couch_total_disk_size (MB)': '{:.2f}'
                                          .format(
                                              getMax(couch_total_disk_size) *
                                              byteToMBConversionFactor
                                          ),
        'mem_used (MB)': '{:.2f}'.format(
                                     getAverage(mem_used) *
                                     byteToMBConversionFactor
                                 ),
        'memcached_rss (MB, all nodes)': '{:.2f}'.format(
                                          getAverageFromList(memcached_rss,
                                                             False) *
                                          byteToMBConversionFactor
                                      ),
        'memcached_cpu': '{:.2f}'.format(getAverageFromList(memcached_cpu,
                                                            True))
    }

    data_list.append(data)

    # Write to JSON
    json_file = output_dir + "/" + project + "-" + number + ".json"
    file = open(json_file, "wb")
    file.write(json.dumps(data))
    file.close()
    print("Data written to " + json_file)

# Write to CSV
csv_file = output_dir + "/" + "data.csv"
file = open(csv_file, "wb")
dictWriter = csv.DictWriter(file, data.keys())
dictWriter.writeheader()
for data in data_list:
    dictWriter.writerow(data)
file.close()
print("Aggregated data for all jobs written to " + csv_file)
