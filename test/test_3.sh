#!/bin/bash

if [ `curl -s http://localhost/protected -w "%{http_code}" -o /dev/null` -ne 401 ] ; then echo "Expected 401"; exit 1;  fi
