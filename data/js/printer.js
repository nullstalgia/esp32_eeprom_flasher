var storage;
var dictionary;
var value;

function load() {
    storage = window.localStorage;
    value = storage.getItem("my-ips");
    console.log("my-ips value:");
    console.log(value);
    if (value == null) {} else {
        dictionary = JSON.parse(value);
        for (var key in dictionary) {
            if (dictionary.hasOwnProperty(key)) {
                console.log(key, dictionary[key]);
                $('#select').append(new Option(key, dictionary[key]))
            }
        }
    }
    document.querySelector('#in').focus();
}

function send(fullSend) {
    var host = "0.0.0.0:80";
    var e = document.querySelector('#in');
    var q = document.querySelector('#qr');
    var c = document.querySelector('#centered');
    var checked;
    if (q.checked) {
        checked = "true";
    } else {
        checked = "false";
    }
    var centered;
    if (c.checked) {
        centered = "true";
    } else {
        centered = "false";
    }
    var json_val = JSON.parse($("#select option:selected").val());
    host = json_val["ip"];
    console.log(host);
    var timeout = 10000;
    if (fullSend) {
        $.ajax({
            type: "POST",
            url: "http://" + host + "/post",
            data: {
                message: e.value,
               qr: checked,
               paw: json_val["paw"],
               centered: centered
            },
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
                    alert("Success!");
                }
            },
        });
        document.querySelector("#log").innerHTML = e.value + "\n" + document.querySelector("#log").innerHTML;
        e.value = '';
    } else {
        var response = "FAIL?";
        $.ajax({
            type: "GET",
            dataType: "text",
            url: "http://" + host + "/printIp",
            timeout: timeout,
            cache: false,
            error: function(jqXHR, textStatus, errorThrown) {
                if (textStatus === "timeout") {
                    alert("Error! Timed out.");
                } else {
                    alert("Error! - " + errorThrown);
                }
                response = errorThrown;
            },
            complete: function(jqXHR, textStatus) {
                if (textStatus === "success") {
                    response = jqXHR.responseText;
                    alert(jqXHR.responseText);
                }
                document.querySelector("#log").innerHTML = "Pinged: " + host + " Response: " + response + "\n" + document.querySelector("#log").innerHTML;
            },
        });
    }
}

function rem_ip() {
    $("#select option:selected").remove();
}

function add_ip() {
    if ($('#port').val() == "") {
        $('#port').val("80");
    }
    var json_val = {};
    json_val["ip"] = $('#ip').val() + ":" + $('#port').val();
    json_val["paw"] = $('#pass').val();
    $('#select').append(new Option($('#name').val(), JSON.stringify(json_val), true, true));
}

function save_ip() {
    dictionary = {};
    $("#select > option").each(function() {
        alert(this.text + ' ' + this.value);
        dictionary[this.text] = this.value;
    });
    console.log(JSON.stringify(dictionary));
    storage.setItem("my-ips", JSON.stringify(dictionary));
}
document.addEventListener('deviceready', setupOpenwith, false);

function setupOpenwith() {
    window.plugins.intent.setNewIntentHandler(function(intent) {
        var subject = intent.extras['android.intent.extra.SUBJECT'];
        var url = intent.extras['android.intent.extra.TEXT'];
        for (var key in intent.extras) {
            if (intent.extras.hasOwnProperty(key)) {
                console.log(key, intent.extras[key]);
            }
        }
        console.log(subject);
        console.log(url);
        if (url != null && url != undefined) {
            document.getElementById("in").value = url;
        }
    });
}
