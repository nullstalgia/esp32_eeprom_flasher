var progress_check_interval;
// Example: var host = "http://192.168.86.40";
var host = "";
var timeout = 5000;
var offset = 0;
var size = 0;
var clear_value = 255;
var max_battery_raw = 2400;
var low_battery_raw = 1900;
var max_battery = 4.2;
var low_battery = 3.5;
var no_battery = 3.3;

function load(is_dump) {
  progress_check();
  progress_check_interval = setInterval(progress_check, 1000);
  //refresh("eeprom");

  refresh("i2c");
  $("#offset").change(hex_offset);

  if (!is_dump) {
    refresh("eeprom");
    //
    refresh("unknown");
    $("#eeprom").change(download_eeprom);

    $("#unknown").change(download_unknown);
  }

  
  if (is_dump) {
    refresh("bin");
    $("#bin").change(download_bin);
    $("#size").change(hex_size);
    $("#clear_value").change(hex_clear_value);
    hex_size();
    hex_clear_value();
  }
  hex_offset();
}

function progress_check() {
  $.getJSON(host + "/progress", function(data) {
    // Battery values were measured with a variable voltage power supply and the raw output of data.battery
    var battery = map(
      data.battery,
      low_battery_raw,
      max_battery_raw,
      low_battery,
      max_battery
    );
    battery = Math.floor(battery * 100) / 100;
    var battery_status;
    if (battery < low_battery + 0.1 && battery > no_battery) {
      battery_status = "Low Battery!";
    }
    var action;
    if (data.action == 0) {
      action = "Ready!";
    } else if (data.action == 1) {
      action = "Uploading to ESP!";
    } else if (data.action == 2) {
      action = "Flashing EEPROM!";
    } else if (data.action == 3) {
      action = "Verifying EEPROM!";
    } else if (data.action == 4) {
      action = "Dumping to Serial!";
    } else if (data.action == 5) {
      action = "Dumping to SPIFFS!";
    } else if (data.action == 6) {
      action = "Running Miniboot action!";
    } else if (data.action == 7) {
      action = "Updating with ArduinoOTA!";
    } else if (data.action == 8) {
      action = "Clearing EEPROM!";
    }
    var result;
    if (data.progress >= 100) {
      if (data.progress == 101) {
        result = "Upload Success";
      } else if (data.progress == 102) {
        result = "Upload Fail";
      } else if (data.progress == 103) {
        result = "Verify Success";
      } else if (data.progress == 104) {
        var decimal = data.fail_byte;
        result = "Verify Fail at D: " + decimal + "  H: " + toHex(decimal);
        // This can be expanded to the other ones easily, but I don't see a reason to.
      } else if (data.progress == 105) {
          result = "Upload to SPIFFS Success";
      } else if (data.progress == 106) {
          result = "Upload to SPIFFS Fail";
      } else if (data.progress == 107) {
        if (data.dump_name != 0) {
          result = "Dumped successfully to: " + data.dump_name + ".eeprom";
        } else {
          result = "Dump Success";
        }
      } else if (data.progress == 108) {
        if (data.dump_name != 0) {
          result =
            "Dump Failed. Some data may be in: " + data.dump_name + ".eeprom";
        } else {
          result = "Dump Fail";
        }
      } else if (data.progress == 109) {
          result = "ArduinoOTA finished. Refresh if it doesn't automatically reconnect";
      } else if (data.progress == 110) {
          result = "ArduinoOTA failed! Please try again or flash over UART";
      }
    } else {
      result = "";
    }

    $("#action").html(action);
    $("#result").html(result);
    var text = `Battery Voltage: ${battery}V`;
    $("#progress").val(data.progress);
    //console.log(data);
    if (battery < no_battery) {
      text = "";
    }
    $("#battery").html(text);
    $("#battery_status").html(battery_status);
  });
}

// https://www.arduino.cc/reference/en/language/functions/math/map/
function map(x, in_min, in_max, out_min, out_max) {
  return ((x - in_min) * (out_max - out_min)) / (in_max - in_min) + out_min;
}

function hex_offset() {
  offset = parseInt($("#offset").val());
  $("#hex_offset").html(toHex(offset));
  console.log(offset);
}

function hex_size() {
  size = parseInt($("#size").val());
  $("#hex_size").html(toHex(size));
  console.log(size);
}

function hex_clear_value() {
  clear_value = parseInt($("#clear_value").val());
  $("#hex_clear_value").html(toHex(clear_value));
  console.log(clear_value);
}

function toHex(decimal) {
  var result = "0x";
  result = result + decimal.toString(16).toUpperCase();
  return result;
}

function getLength() {
  hex_offset();
  var i2caddress = $("#i2c option:selected").val();
  if (i2caddress != undefined) {
    console.log(
      host + "/get_minibyte_length?address=" + i2caddress + "&offset=" + offset
    );
    $.getJSON(
      host + "/get_minibyte_length?address=" + i2caddress + "&offset=" + offset,
      function(data) {
        if (data.success) {
          if (confirm("Do you want to include the miniboot header?")) {
            $("#size").val(data.length + 34);
            hex_size();
          } else {
            $("#size").val(data.length);
            $("#offset").val(offset + 34);
            hex_offset();
            hex_size();
          }
          alert("Length has been set! Dump when ready.");
        } else {
          alert("Is not a proper miniboot eeprom?");
        }
      }
    );
  } else {
    alert("You have to choose an I2C address!");
  }
}

function getCRC(source) {
  if (source == "eeprom") {
    hex_offset();
    var i2caddress = $("#i2c option:selected").val();
    if (i2caddress != undefined) {
      $.getJSON(
        host + "/get_minicrc?address=" + i2caddress + "&offset=" + offset,
        function(data) {
          if (data.success) {
            var data =
              "(EEPROM) CRC32 from Miniboot Header: " + data.written_crc;
            alert(data);
            console.log(data);
          } else {
            alert("Is not a proper miniboot eeprom?");
          }
        }
      );
    } else {
      alert("You have to choose an I2C address!");
    }
  } else if (source == "spiffs") {
    var file = $("#eeprom option:selected").val();
    if (file != undefined) {
      $.getJSON(host + "/get_spiffs_minicrc?file=" + file, function(data) {
        if (data.success) {
          var data = "(SPIFFS) CRC32 from Miniboot Header: " + data.written_crc;
          alert(data);
          console.log(data);
        } else {
          alert("Is not a proper miniboot eeprom?");
        }
      });
    } else {
      alert("You have to choose an EEPROM file!");
    }
  }
}

function dump(destination) {
  hex_offset();
  hex_size();
  var i2caddress = $("#i2c option:selected").val();
  if (size > 0) {
    if (i2caddress != undefined) {
      //console.log(file);
      console.log(i2caddress);

      $.ajax({
        type: "GET",
        url: host + "/dump_eeprom_" + destination,
        timeout: timeout,
        cache: false,
        data: {
          address: i2caddress,
          size: size,
          offset: offset
        },
        error: function(jqXHR, textStatus, errorThrown) {
          if (textStatus === "timeout") {
            alert("Error! Timed out.");
          } else {
            alert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            alert("Action sent: Dump to " + destination);
          }
        }
      });
    } else {
      alert("You have to choose an I2C address!");
    }
  } else {
    alert("You have to choose a size greater than Zero (0)!");
  }
}

function clear_eeprom() {
    hex_offset();
    hex_size();
    hex_clear_value();
    var i2caddress = $("#i2c option:selected").val();
    if (size > 0) {
        if (i2caddress != undefined) {
            if (confirm("Are you sure you want to clear with the selected values?")) {
                //console.log(file);
                console.log(i2caddress);
                
                $.ajax({
                    type: "GET",
                    url: host + "/clear_eeprom",
                    timeout: timeout,
                    cache: false,
                    data: {
                        address: i2caddress,
                        size: size,
                        offset: offset,
                        clear_value: clear_value
                    },
                    error: function(jqXHR, textStatus, errorThrown) {
                        if (textStatus === "timeout") {
                            alert("Error! Timed out.");
                        } else {
                            alert("Error! - " + errorThrown);
                        }
                    },
                    complete: function(jqXHR, textStatus) {
                        if (textStatus === "success") {
                            alert("Action sent: Dump to " + destination);
                        }
                    }
                });
            }
        } else {
            alert("You have to choose an I2C address!");
        }
    } else {
        alert("You have to choose a size greater than Zero (0)!");
    }
}

function eeprom_action(action) {
  hex_offset();
  hex_size();
  var file = $("#eeprom option:selected").val();
  var i2caddress = $("#i2c option:selected").val();
  if (file != undefined) {
    if (i2caddress != undefined) {
      console.log(file);
      console.log(i2caddress);

      $.ajax({
        type: "GET",
        url: host + "/" + action + "_eeprom",
        timeout: timeout,
        cache: false,
        data: {
          file: '"' + file + '"',
          address: i2caddress,
          offset: offset
        },
        error: function(jqXHR, textStatus, errorThrown) {
          if (textStatus === "timeout") {
            alert("Error! Timed out.");
          } else {
            alert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            alert("Action sent: " + action);
          }
        }
      });
    } else {
      alert("You have to choose an I2C address!");
    }
  } else {
    alert("You have to choose an EEPROM file!");
  }
}

function delete_file(section) {
  var file = $("#" + section + " option:selected").val();
  if (file != undefined) {
    if (confirm("Are you sure you want to delete: " + file)) {
      console.log(file);

      $.ajax({
        type: "GET",
        url: host + "/delete_file",
        timeout: timeout,
        cache: false,
        data: {
          file: '"' + file + '"'
        },
        error: function(jqXHR, textStatus, errorThrown) {
          if (textStatus === "timeout") {
            alert("Error! Timed out.");
          } else {
            alert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            alert("Deleted.");
          }
        }
      });

      refresh(section);
    }
  } else {
    alert("You have to choose a file!");
  }
}

function download_eeprom(section) {
  var file = $("#eeprom option:selected").val();
  if (file != undefined) {
    $("#eeprom_download").attr("href", "/fs" + file);
    console.log(file);
  }
}
function download_bin(section) {
  var file = $("#bin option:selected").val();
  if (file != undefined) {
    $("#bin_download").attr("href", "/fs" + file);
    console.log(file);
  }
}
function download_unknown(section) {
  var file = $("#unknown option:selected").val();
  if (file != undefined) {
    $("#unknown_download").attr("href", "/fs" + file);
    console.log(file);
  }
}

function refresh(type) {
  $("#" + type + " option").remove();
  if (type == "i2c") {
    $.ajax({
      type: "GET",
      url: host + "/i2c_devices",
      timeout: timeout,
      cache: false,
      error: function(jqXHR, textStatus, errorThrown) {
        if (textStatus === "timeout") {
          alert("Error! Timed out.");
        } else {
          alert("Error! - " + errorThrown);
        }
      },
      complete: function(jqXHR, textStatus) {
        if (textStatus === "success") {
          var devices = jqXHR.responseJSON;
          if (devices.length > 0) {
            var i;
            for (i = 0; i < devices.length; i++) {
              var decimal = devices[i];
              $("#i2c").append(new Option(toHex(decimal), decimal));
              //console.log(hex);
            }
          }
        }
      }
    });
    //document.querySelector("#log").innerHTML = e.value + "\n" + document.querySelector("#log").innerHTML;
    //} else if (type == "bin" || type == "eeprom" || type == "unknown"){
  } else {
    $.ajax({
      type: "GET",
      url: host + "/refresh?dir=" + type,
      timeout: timeout,
      cache: false,
      error: function(jqXHR, textStatus, errorThrown) {
        if (textStatus === "timeout") {
          alert("Error! Timed out.");
        } else {
          alert("Error! - " + errorThrown);
        }
      },
      complete: function(jqXHR, textStatus) {
        if (textStatus === "success") {
          var files = jqXHR.responseJSON["files"];
          if (files.length > 0) {
            console.log(files);
            var i;
            for (i = 0; i < files.length; i++) {
              $("#" + type).append(
                new Option(files[i][0] + " - bytes:" + files[i][1], files[i][0])
              );
            }
          }
        }
      }
    });
    //document.querySelector("#log").innerHTML = e.value + "\n" + document.querySelector("#log").innerHTML;
  }
}
