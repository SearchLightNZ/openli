## Mediator Configuration

Like all OpenLI components, the mediator uses YAML as its configuration
file format. If you are unfamiliar with YAML, a decent crash course is
available [here](https://learnxinyminutes.com/docs/yaml/).

An example configuration file (with in-line documentation) can found in
`doc/exampleconfigs/mediator-example.yaml`.

### Operator ID
This option should contain a string that uniquely identifies your network.
This is a mandatory field for ETSI compliance and will be used to set the
operatorIdentifier field in the ETSI header. The operator ID should be no
more than 16 characters in length. The operator ID specified here is
used to populate ETSI keep-alive messages that maintain the connection
between your mediator and the law enforcement agencies.

Ideally, the operator ID configured for your mediator(s) should match
the operator ID that you are using for your collectors.

### Mediator ID
Each mediator that you are running needs to be assigned a unique mediator
ID. The mediator ID should be a number between 0 and 1,000,000.

Most deployments will only have one mediator, so this shouldn't be too
difficult to keep track of. Remember your mediator ID, as you will need
it when starting an intercept.

### Listening Socket
The listen address and port options are used to describe which address and
port (on the mediator host) should listen for incoming connections from the
collector(s).

Make sure you actually choose an IP address that is assigned to the mediator
host!

### Provisioner Socket
The provisioner address and port options describe how to connect to the
host that the OpenLI provisioner is running on. If the mediator cannot
connect to the provisioner, it will not be able to announce itself as being
available and therefore no collectors will be told to connect to it. If
the provisioner goes down for some reason, the mediator will periodically
attempt to reconnect to it.

### Pcap Directory
OpenLI allows intercepts to be written to disk as pcap trace files instead
of being live streamed to the requesting agency. If you wish to do this for
any intercepts, you will need to set the pcap directory option in your
mediator configuration. All pcap traces created by this mediator will be
written into this directory; filenames will include the LIID for the intercept
so should be unique and easily identifiable.

The pcap output files will be rotated every 30 minutes. If no traffic is
observed for that intercept during the 30 minute period, no output file will
be created. The rotation frequency can be configured.

Note: a pcap file should not be considered usable until *after* it has been
rotated -- in-progress pcap traces do not contain all of the necessary
trailers to allow them to be correctly parsed by a reader.

### Configuration Syntax
All of the mediator config options are standard YAML key-value pairs, where
the key is the option name and the value is your chosen value for that option.

The supported option keys are:
* operatorid       -- set the operator ID
* mediatorid       -- sets the mediator ID number
* provisioneraddr  -- connect to a provisioner at this IP address
* provisionerport  -- connect to a provisioner listening on this port
* listenaddr       -- listen on the interface with this address for collectors
* listenport       -- listen on this port for collectors
* pcapdirectory    -- the directory to write any pcap trace files to
* pcaprotatefreq   -- the number of minutes to wait before rotating pcap traces
