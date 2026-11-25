const protocol = require('./lib/protocol');
const { SRRClient, SRRUDPClient } = require('./lib/client');

module.exports = {
  SRRClient,
  SRRUDPClient,
  ...protocol
};
