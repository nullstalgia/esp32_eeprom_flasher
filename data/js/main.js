var progress_check_interval;
// Example:
var host = "";
//host = "http://192.168.86.40";
var timeout = 5000;
var offset = 0;
var size = 0;
var clear_value = 255;
var max_battery_raw = 2400;
var low_battery_raw = 1900;
var max_battery = 4.2;
var low_battery = 3.5;
var no_battery = 3.3;
var alert_index = 0;
var battery_value_array = [];
var battery_index = 0;
var battery_percent = 0;
function load() {
  progress_check();
  progress_check_interval = setInterval(progress_check, 1000);
  //refresh("eeprom");

  refresh("i2c");
  $("#offset").change(hex_offset);

  refresh("eeprom");
  //
  refresh("unknown");
  $("#eeprom").change(download_eeprom);

  $("#unknown").change(download_unknown);

  refresh("bin");
  $("#bin").change(download_bin);
  $("#size").change(hex_size);
  $("#clear_value").change(hex_clear_value);
  hex_size();
  hex_clear_value();
  $("#hex_size").change(hexSizeToDecimal);
  $("#hex_clear_value").change(hexValueToDecimal);

  hex_offset();
  $("#hex_offset").change(hexOffsetToDecimal);
}

function preferences_load(){
  load();
  get_pref();
}

function get_pref(){
  showalert("Getting current preferences", "info");
  $.getJSON(host + "/get_pref", function(data) {
    console.log(data);
    $("#use_pages").prop('checked', data.use_pages);
    $("#size").val(data.page_size);
    hex_size();
    $("#max_att").val(data.max_att);
    $("#delay").val(data.delay);
    $("#req_delay").val(data.req_delay);

    showalert("Loaded!", "success")
  });
}

function set_pref() {
  hex_size();
  var use_pages = $('#use_pages').is(':checked');
  if(use_pages){
    use_pages = 1;
  } else {
    use_pages = 0;
  }
  var page_size = size;
  var max_att = $("#max_att").val();
  var delay = $("#delay").val();
  var req_delay = $("#req_delay").val();
  if (page_size > -1) {
    if(max_att < 1){
      showalert("Having a Max Attempt < 1 won't do anything! Allowing anyway.", "warning");
    }
    if(delay < 0){
      showalert("Delay can't be negative!");
      return;
    }
    if(req_delay < 0){
      showalert("RequestFrom Delay can't be negative!");
      return;
    }
    if (use_pages != undefined) {
      //console.log(file);
      $.ajax({
        type: "GET",
        url: host + "/set_pref",
        timeout: timeout,
        cache: false,
        data: {
          use_pages: use_pages,
          page_size: page_size,
          max_att: max_att,
          delay: delay,
          req_delay: req_delay
        },
        error: function(jqXHR, textStatus, errorThrown) {
          if (textStatus === "timeout") {
            showalert("Error! Timed out.");
          } else {
            showalert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            showalert("", "success");
          }
        }
      });
    } else {
      showalert("Error with the checkbox!");
    }
  } else {
    showalert("You have to choose a size greater than -1!");
  }
}

function progress_check() {
  $.getJSON(host + "/progress", function(data) {
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
    } else if (data.action == 9) {
      action = "Updating preferences!";
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
        result = "Verify Fail at D: " + decimal + "  H: 0x" + toHex(decimal);
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
        result =
          "ArduinoOTA finished. Refresh if it doesn't automatically reconnect";
      } else if (data.progress == 110) {
        result = "ArduinoOTA failed! Please try again or flash over UART";
      }
    } else {
      result = "";
    }

    $("#action").html(action);
    $("#result").html(result);
    $("#progress_percent").width(data.progress + "%");
    var percent = data.progress;

    $("#progress_percent").removeClass("bg-info bg-danger bg-success");
    if (percent > 100) {
      if (percent % 2 == 0) {
        $("#progress_percent").addClass("bg-danger");
      } else {
        $("#progress_percent").addClass("bg-success");
      }
      percent = 100;
    } else {
      $("#progress_percent").addClass("bg-info");
    }
    $("#progress_percent").html(percent + "%");

    // Battery values were measured with a variable voltage power supply and the raw output of data.battery
    var battery = map(
      data.battery,
      low_battery_raw,
      max_battery_raw,
      low_battery,
      max_battery
    );

    //console.log(data);
    if (battery < no_battery) {
      $(".battery").css("display", "none");
    } else {
      //$(".battery").css("display", "flex");

      battery_value_array[battery_index] = battery;
      battery_index++;
      if (battery_index >= 25) {
        battery_index = 0;
      }
      var battery_average = 0;
      for (let i = 0; i < battery_value_array.length; i++) {
        battery_average += battery_value_array[i];
      }
      battery = battery_average / battery_value_array.length;
      battery = Math.floor(battery * 100) / 100;
      var battery_text = battery + "V";
      $("#battery").html(battery_text);
      battery_percent = map(battery, low_battery, max_battery, 0, 100);
      $("#battery").width(battery_percent + "%");
      $("#battery").removeClass("bg-warning bg-danger bg-success");
      if (battery_percent > (2 / 3) * 100) {
        $("#battery").addClass("bg-success");
      } else if (battery_percent > (1 / 3) * 100) {
        $("#battery").addClass("bg-warning");
      } else {
        $("#battery").addClass("bg-danger");
      }
    }
  });
}

// https://www.arduino.cc/reference/en/language/functions/math/map/
function map(x, in_min, in_max, out_min, out_max) {
  return ((x - in_min) * (out_max - out_min)) / (in_max - in_min) + out_min;
}

function hex_offset() {
  offset = parseInt($("#offset").val());
  $("#hex_offset").val(toHex(offset));
  console.log(offset);
}

function hex_size() {
  size = parseInt($("#size").val());
  $("#hex_size").val(toHex(size));
  console.log(size);
}

function hex_clear_value() {
  clear_value = parseInt($("#clear_value").val());
  $("#hex_clear_value").val(toHex(clear_value));
  console.log(clear_value);
}

function toHex(decimal) {
  var result = "";
  result = result + decimal.toString(16).toUpperCase();
  return result;
}

function hexOffsetToDecimal() {
  var input = $("#hex_offset").val();
  $("#offset").val(parseInt(input, 16));
}

function hexSizeToDecimal() {
  var input = $("#hex_size").val();
  $("#size").val(parseInt(input, 16));
}

function hexValueToDecimal() {
  var input = $("#hex_clear_value").val();
  $("#clear_value").val(parseInt(input, 16));
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
          showalert("Length has been set! Dump when ready.", "info");
        } else {
          showalert("Is not a proper miniboot eeprom?", "warning");
        }
      }
    );
  } else {
    showalert("You have to choose an I2C address!");
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
            showalert("Is not a proper miniboot eeprom?", "warning");
          }
        }
      );
    } else {
      showalert("You have to choose an I2C address!");
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
          showalert("Is not a proper miniboot eeprom?", "warning");
        }
      });
    } else {
      showalert("You have to choose an EEPROM file!");
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
            showalert("Error! Timed out.");
          } else {
            showalert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            showalert("Action sent: Dump to " + destination, "success");
          }
        }
      });
    } else {
      showalert("You have to choose an I2C address!");
    }
  } else {
    showalert("You have to choose a size greater than Zero (0)!");
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
              showalert("Error! Timed out.");
            } else {
              showalert("Error! - " + errorThrown);
            }
          },
          complete: function(jqXHR, textStatus) {
            if (textStatus === "success") {
              showalert(`Clear action sent. ${size} bytes`, "success");
            }
          }
        });
      }
    } else {
      showalert("You have to choose an I2C address!");
    }
  } else {
    showalert("You have to choose a size greater than Zero (0)!");
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
            showalert("Error! Timed out.");
          } else {
            showalert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            //alert("Action sent: " + action);
            showalert("Action sent: " + action, "success");
          }
        }
      });
    } else {
      showalert("You have to choose an I2C address!");
    }
  } else {
    showalert("You have to choose an EEPROM file!");
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
            showalert("Error! Timed out.");
          } else {
            showalert("Error! - " + errorThrown);
          }
        },
        complete: function(jqXHR, textStatus) {
          if (textStatus === "success") {
            showalert("Deleted.", "success");
          }
        }
      });

      refresh(section);
    }
  } else {
    showalert("You have to choose a file!");
  }
}

// Unfortunately, there's no way for me to pass an argument to this.... As far as I know
function download_eeprom() {
  var file = $("#eeprom option:selected").val();
  if (file != undefined) {
    $("#eeprom_download").attr("href", "/fs" + file);
    console.log(file);
  }
}
function download_bin() {
  var file = $("#bin option:selected").val();
  if (file != undefined) {
    $("#bin_download").attr("href", "/fs" + file);
    console.log(file);
  }
}
function download_unknown() {
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
          showalert("Error! Timed out.");
        } else {
          showalert("Error! - " + errorThrown);
        }
      },
      complete: function(jqXHR, textStatus) {
        if (textStatus === "success") {
          var devices = jqXHR.responseJSON;
          if (devices.length > 0) {
            var i;
            for (i = 0; i < devices.length; i++) {
              var decimal = devices[i];
              $("#i2c").append(new Option("0x" + toHex(decimal), decimal));
              if (i == 0) {
                $("#i2c option").prop("selected", true);
              }
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
          showalert("Error! Timed out.");
        } else {
          showalert("Error! - " + errorThrown);
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
                new Option(
                  files[i][0].split("/").pop() + " - bytes:" + files[i][1],
                  files[i][0]
                )
              );
            }
          }
        }
      }
    });
    //document.querySelector("#log").innerHTML = e.value + "\n" + document.querySelector("#log").innerHTML;
  }
}
/**
  Bootstrap Alerts -
  Function Name - showalert()
  Inputs - message,alerttype
  Example - showalert("Invalid Login","alert-error")
  Types of alerts -- "alert-danger","alert-success","alert-info","alert-warning"
  Required - You only need to add a alert_placeholder div in your html page wherever you want to display these alerts "<div id="alert_placeholder"></div>"
  Written On - 14-Jun-2013
**/
// https://stackoverflow.com/a/17118264/6037497
function showalert(message, alerttype = "danger") {
  $("#alert_placeholder").append(
    '<div id="alertdiv' +
      alert_index +
      '" class="alert alert-' +
      alerttype +
      ' show fade"><a class="close" data-dismiss="alert">×</a><span>' +
      message +
      "</span></div>"
  );
  //style="display:none;"
  //$("#alertdiv").fadeIn();
  setTimeout(function() {
    // this will automatically close the alert and remove this if the users doesnt close it in 5 secs
    $("#alertdiv" + alert_index).alert("close");
  }, 3000);
}
