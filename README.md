# ULP rain gauge application

This software count the pulse of rain gauge output and send those values to
Fluentd.

By using low power ULP, it can powered by AAA batteries.

# Pin assign
* GPIO25: Rain gauge (pull down)
* GPIO32: Battery voltage monitor (A/D)
