#!/bin/bash
for i in $(seq 1 10000); do echo $i; curl -I -0 -X GET "http://127.0.0.1:12345/index.html"; done
