/**
 * List of functions to execute when a button is clicked on the GUI.
 */
var action_socket = null;


/**
 * Effectively the main function
 */
$(document).ready(function () {
  /**
   * On document ready, disable every button on the GUI until released by the
   * main server, also, for every button pressed, disable all buttons until the
   * board type is released. Release signal will be handled by server side.
   */
  $('button').each(function () {
    $(this).prop('disabled', true);
    $(this).on('click', function () {
      $('button').each(function () {
        $(this).prop('disabled', true);
      });
    });
  });


  action_socket = io.connect("http://localhost:9100/action");

  action_socket.on('connect', function () {
    console.log('action socket connected!');
  });

  action_socket.on('action-received', function (msg) {
    // Disable every button on start up
    $('button').each(function () {
      $(this).prop('disabled', true);
    });
  });

  action_socket.on('action-complete', function () {
    console.log('action-complete recived!');
    $('button').each(function () {
      $(this).prop('disabled', false);
    });
  });

  $('#standard-d8').on('click', function () {
    emit_action_cmd($(this).prop('id'), { 'section': 'this is a test' })
    console.log('Standard-d8 activated');
  })

  $('#raw-cmd-input').on('click', function() {
    var line = $(this).siblings('#raw-cmd-input-text').val();
    emit_action_cmd($(this).prop('id'), {
      'input' : line
    });
  })
});


function emit_action_cmd(id, msg) {
  action_socket.emit('run-action-cmd', {
    'id': id,
    'data': msg
  });
}