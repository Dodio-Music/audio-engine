const addon = require('./build/Release/dodio_audio.node');


module.exports = addon;
module.exports.AudioPlayer = addon.AudioPlayer;
