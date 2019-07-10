# Processor Scraper
This C project is for the SEDS BPHC chapter inductions. 

This applications displays system info including CPU utilization and RAM use every second. It also serializes the CPU utilization and details of the top 10 memory hungry proccess into JSON and POSTs this data to this [endpoint](https://fathomless-thicket-66026.herokuapp.com/viswa).

## Libraries
The project uses the following libraries: 

1. cURL
2. json-c
   
You can compile the project yourself using the provided makefile.

## Portability
__cURL__: Most Linux distros should have cURL installed by default. If not, you can use your package manager to install it. 

__json-c__: This library is less frequent. You may install it from by cloning their [GitHub Repo](https://github.com/json-c/json-c). 

However, the executable was linked with rpath provided and the loader should be configured to look for shared libraries in the _lib/_ directory relative to the executable.



