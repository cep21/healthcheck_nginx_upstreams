#!/bin/bash

perl util/wiki2pod.pl README.wiki > /tmp/a.pod && pod2text /tmp/a.pod > README
