<?php
// $Id$

define('SCHEMA_UNINSTALLED', -1);
define('SCHEMA_INSTALLED', 0);

define('DRUPAL_MINIMUM_PHP',    '4.3.3');
define('DRUPAL_MINIMUM_MEMORY', '8M');
define('DRUPAL_MINIMUM_MYSQL',  '3.23.17'); // If using MySQL
define('DRUPAL_MINIMUM_PGSQL',  '7.3');  // If using PostgreSQL
define('DRUPAL_MINIMUM_APACHE', '1.3');  // If using Apache

define('FILE_EXIST',          1);
define('FILE_READABLE',       2);
define('FILE_WRITABLE',       4);
define('FILE_EXECUTABLE',     8);
define('FILE_NOT_EXIST',      16);
define('FILE_NOT_READABLE',   32);
define('FILE_NOT_WRITABLE',   64);
define('FILE_NOT_EXECUTABLE', 128);

/**
 * Initialize the update system by loading all installed module's .install files.
 */
function drupal_load_updates() {
  foreach (module_list() as $module) {
    module_load_install($module);
  }
}

/**
 * Returns an array of available schema versions for a module.
 *
 * @param $module
 *   A module name.
 * @return
 *   If the module has updates, an array of available updates. Otherwise,
 *   FALSE.
 */
function drupal_get_schema_versions($module) {
  $functions = get_defined_functions();
  foreach ($functions['user'] as $function) {
    if (strpos($function, $module .'_update_') === 0) {
      $version = substr($function, strlen($module .'_update_'));
      if (is_numeric($version)) {
        $updates[] = $version;
      }
    }
  }
  if (count($updates) == 0) {
    return FALSE;
  }
  return $updates;
}

/**
 * Returns the currently installed schema version for a module.
 *
 * @param $module
 *   A module name.
 * @return
 *   The currently installed schema version.
 */
function drupal_get_installed_schema_version($module, $reset = FALSE) {
  static $versions;

  if ($reset) {
    unset($versions);
  }

  if (!$versions) {
    $versions = array();
    $result = db_query("SELECT name, schema_version FROM {system} WHERE type = 'module'");
    while ($row = db_fetch_object($result)) {
      $versions[$row->name] = $row->schema_version;
    }
  }

  return $versions[$module];
}

/**
 * Update the installed version information for a module.
 *
 * @param $module
 *   A module name.
 * @param $version
 *   The new schema version.
 */
function drupal_set_installed_schema_version($module, $version) {
  db_query("UPDATE {system} SET schema_version = %d WHERE name = '%s'", $version, $module);
}

/**
 * Loads the profile definition, extracting the profile's defined name.
 *
 * @return
 *   The name defined in the profile's _profile_details() hook.
 */
function drupal_install_profile_name() {
  global $profile;
  static $name = NULL;

  if (!isset($name)) {
    // Load profile details.
    $function = $profile .'_profile_details';
    if (function_exists($function)) {
      $details = $function();
    }
    $name = isset($details['name']) ? $details['name'] : 'Drupal';
  }

  return $name;
}

/**
 * Auto detect the base_url with PHP predefined variables.
 *
 * @param $file
 *   The name of the file calling this function so we can strip it out of
 *   the URI when generating the base_url.
 *
 * @return
 *   The auto-detected $base_url that should be configured in settings.php
 */
function drupal_detect_baseurl($file = 'install.php') {
  global $profile;
  $proto = $_SERVER['HTTPS'] ? 'https://' : 'http://';
  $host = $_SERVER['SERVER_NAME'];
  $port = ($_SERVER['SERVER_PORT'] == 80 ? '' : ':'. $_SERVER['SERVER_PORT']);
  $uri = str_replace("?profile=$profile", '', $_SERVER['REQUEST_URI']);
  $dir = str_replace("/$file", '', $uri);

  return "$proto$host$port$dir";
}

/**
 * Detect all databases supported by Drupal that are compiled into the current
 * PHP installation.
 *
 * @return
 *  An array of database types compiled into PHP.
 */
function drupal_detect_database_types() {
  $databases = array();

  foreach (array('mysql', 'mysqli', 'pgsql') as $type) {
    if (file_exists('./includes/install.'. $type .'.inc')) {
      include_once './includes/install.'. $type .'.inc';
      $function = $type .'_is_available';
      if ($function()) {
        $databases[$type] = $type;
      }
    }
  }

  return $databases;
}

/**
 * Read settings.php into a buffer line by line, changing values specified in
 * $settings array, then over-writing the old settings.php file.
 *
 * @param $settings
 *   An array of settings that need to be updated.
 */
function drupal_rewrite_settings($settings = array(), $prefix = '') {
  $settings_file = './'. conf_path() .'/'. $prefix .'settings.php';

  // Build list of setting names and insert the values into the global namespace.
  $keys = array();
  foreach ($settings as $setting => $data) {
    $GLOBALS[$setting] = $data['value'];
    $keys[] = $setting;
  }

  $buffer = NULL;
  $first = TRUE;
  if ($fp = @fopen($settings_file, 'r+')) {
    // Step line by line through settings.php.
    while (!feof($fp)) {
      $line = fgets($fp);
      if ($first && substr($line, 0, 5) != '<?php') {
        $buffer = "<?php\n\n";
      }
      $first = FALSE;
      // Check for constants.
      if (substr($line, 0, 7) == 'define(') {
        preg_match('/define\(\s*[\'"]([A-Z_-]+)[\'"]\s*,(.*?)\);/', $line, $variable);
        if (in_array($variable[1], $keys)) {
          $setting = $settings[$variable[1]];
          $buffer .= str_replace($variable[2], " '". $setting['value'] ."'", $line);
          unset($settings[$variable[1]]);
          unset($settings[$variable[2]]);
        }
        else {
          $buffer .= $line;
        }
      }
      // Check for variables.
      elseif (substr($line, 0, 1) == '$') {
        preg_match('/\$([^ ]*) /', $line, $variable);
        if (in_array($variable[1], $keys)) {
          // Write new value to settings.php in the following format:
          //    $'setting' = 'value'; // 'comment'
          $setting = $settings[$variable[1]];
          $buffer .= '$'. $variable[1] ." = '". $setting['value'] ."';". ($setting['comment'] ? ' // '. $setting['comment'] ."\n" : "\n");
          unset($settings[$variable[1]]);
        }
        else {
          $buffer .= $line;
        }
      }
      else {
        $buffer .= $line;
      }
    }
    fclose($fp);

    // Add required settings that were missing from settings.php.
    foreach ($settings as $setting => $data) {
      if ($data['required']) {
        $buffer .= "\$$setting = '". $data['value'] ."';\n";
      }
    }

    $fp = fopen($settings_file, 'w');
    if ($fp && fwrite($fp, $buffer) === FALSE) {
        drupal_set_message(st('Failed to modify %settings, please verify the file permissions.', array('%settings' => $settings_file)), 'error');
    }
  }
  else {
    drupal_set_message(st('Failed to open %settings, please verify the file permissions.', array('%settings' => $settings_file)), 'error');
  }
}

/**
 * Get list of all .install files.
 *
 * @param $module_list
 *   An array of modules to search for their .install files.
 */
function drupal_get_install_files($module_list = array()) {
  $installs = array();
  foreach ($module_list as $module) {
    $installs = array_merge($installs, file_scan_directory('./modules', "^$module.install$", array('.', '..', 'CVS'), 0, TRUE, 'name', 0));
  }
  return $installs;
}

/**
 * Verify a profile for installation.
 *
 * @param profile
 *   Name of profile to verify.
 * @return
 *   The list of modules to install.
 */
function drupal_verify_profile($profile) {
  include_once './includes/file.inc';

  $profile_file = "./profiles/$profile/$profile.profile";

  if (!isset($profile) || !file_exists($profile_file)) {
    _install_no_profile_error();
  }

  require_once($profile_file);

  // Get a list of modules required by this profile.
  $function = $profile .'_profile_modules';
  $module_list = array_merge(array('system'), $function());

  // Verify that all required modules exist.
  $modules_present = TRUE;
  foreach ($module_list as $module) {
    $module_path = dirname(drupal_get_filename('module', $module, NULL, FALSE));
    if (!$module_path) {
      drupal_set_message(st('The %module module is required but was not found. Please move it into the <em>modules</em> subdirectory.', array('%module' => $module)), 'error');
      $modules_present = FALSE;
    }
  }

  return $modules_present ? $module_list : NULL;
}

/**
 * Install a profile (i.e. a set of modules) from scratch.
 * The profile must be verified first using drupal_verify_profile().
 *
 * @param profile
 *   The name of the profile to install.
 * @param module_list
 *   An array of modules to install.
 */
function drupal_install_profile($profile, $module_list) {
  // The system module is a special case; we can't bootstrap until it's
  // installed, so we can't use the normal installation function.
  $module_list = array_diff($module_list, array('system'));

  $system_path = dirname(drupal_get_filename('module', 'system', NULL, FALSE));
  require_once './' . $system_path . '/system.install';
  module_invoke('system', 'install');
  $system_versions = drupal_get_schema_versions('system');
  $system_version = $system_versions ? max($system_versions) : SCHEMA_INSTALLED;
  db_query("INSERT INTO {system} (filename, name, type, description, status, throttle, bootstrap, schema_version) VALUES('%s', '%s', 'module', '', 1, 0, 0, %d)", $system_path . '/system.module', 'system', $system_version);

  // Now that we've installed things properly, bootstrap the full Drupal environment
  drupal_bootstrap(DRUPAL_BOOTSTRAP_FULL);

  // Install schemas for profile and all its modules.
  module_rebuild_cache();
  drupal_install_modules($module_list);

  // And now, run the profile's install function.
  $function = $profile .'_install';
  if (function_exists($function)) {
    $function();
  }
}


/**
 * Execute the install scripts for a set of modules.
 *
 * @param module_list
 *   The modules to install.
 */
function drupal_install_modules($module_list = array()) {
  foreach ($module_list as $module) {
    drupal_install_module($module);
  }
}

/**
 * Calls the install function and updates the system table for a given module.
 *
 * @param module
 *   The module to install.
 */
function drupal_install_module($module) {
  if (drupal_get_installed_schema_version($module, TRUE) == SCHEMA_UNINSTALLED) {
    module_load_install($module);
    module_invoke($module, 'install');
    $versions = drupal_get_schema_versions($module);
    drupal_set_installed_schema_version($module, $versions ? max($versions) : SCHEMA_INSTALLED);
    module_enable($module);
  }
}

/**
 * Verify the state of the specified file.
 *
 * @param $file
 *   The file to check for.
 * @param $mask
 *   An optional bitmask created from various FILE_* constants.
 * @param $type
 *   The type of file. Can be file (default), dir, or link.
 * @return
 *   TRUE on success or FALSE on failure. A messsage is set for the latter.
 */
function drupal_verify_install_file($file, $mask = NULL, $type = 'file') {
  $return = TRUE;
  // Check for files that shouldn't be there.
  if (isset($mask) && ($mask & FILE_NOT_EXIST) && file_exists($file)) {
    return FALSE;
  }
  // Verify that the file is the type of file it is supposed to be.
  if (isset($type) && file_exists($file)) {
    $check = 'is_'. $type;
    if (!function_exists($check) || !$check($file)) {
      $return = FALSE;
    }
  }

  // Verify file permissions.
  if (isset($mask)) {
    $masks = array(FILE_EXIST, FILE_READABLE, FILE_WRITABLE, FILE_EXECUTABLE, FILE_NOT_READABLE, FILE_NOT_WRITABLE, FILE_NOT_EXECUTABLE);
    foreach ($masks as $current_mask) {
      if ($mask & $current_mask) {
        switch ($current_mask) {
          case FILE_EXIST:
            if (!file_exists($file)) {
              if ($type == 'dir') {
                drupal_install_mkdir($file, $mask);
              }
              if (!file_exists($file)) {
                $return = FALSE;
              }
            }
            break;
          case FILE_READABLE:
            if (!is_readable($file) && !drupal_install_fix_file($file, $mask)) {
              $return = FALSE;
            }
            break;
          case FILE_WRITABLE:
            if (!is_writable($file) && !drupal_install_fix_file($file, $mask)) {
              $return = FALSE;
            }
            break;
          case FILE_EXECUTABLE:
            if (!is_executable($file) && !drupal_install_fix_file($file, $mask)) {
              $return = FALSE;
            }
            break;
          case FILE_NOT_READABLE:
            if (is_readable($file) && !drupal_install_fix_file($file, $mask)) {
              $return = FALSE;
            }
            break;
          case FILE_NOT_WRITABLE:
            if (is_writable($file) && !drupal_install_fix_file($file, $mask)) {
              $return = FALSE;
            }
            break;
          case FILE_NOT_EXECUTABLE:
            if (is_executable($file) && !drupal_install_fix_file($file, $mask)) {
              $return = FALSE;
            }
            break;
        }
      }
    }
  }
  return $return;
}

/**
 * Create a directory with specified permissions.
 *
 * @param file
 *  The name of the directory to create;
 * @param mask
 *  The permissions of the directory to create.
 * @param $message
 *  (optional) Whether to output messages. Defaults to TRUE.
 *
 * @return
 *  TRUE/FALSE whether or not the directory was successfully created.
 */
function drupal_install_mkdir($file, $mask, $message = TRUE) {
  $mod = 0;
  $masks = array(FILE_READABLE, FILE_WRITABLE, FILE_EXECUTABLE, FILE_NOT_READABLE, FILE_NOT_WRITABLE, FILE_NOT_EXECUTABLE);
  foreach ($masks as $m) {
    if ($mask & $m) {
      switch ($m) {
        case FILE_READABLE:
          $mod += 444;
          break;
        case FILE_WRITABLE:
          $mod += 222;
          break;
        case FILE_EXECUTABLE:
          $mod += 111;
          break;
      }
    }
  }

  if (@mkdir($file, intval("0$mod", 8))) {
    return TRUE;
  }
  else {
    return FALSE;
  }
}

/**
 * Attempt to fix file permissions.
 *
 * @param $file
 *  The name of the file with permissions to fix.
 * @param $mask
 *  The desired permissions for the file.
 * @param $message
 *  (optional) Whether to output messages. Defaults to TRUE.
 *
 * @return
 *  TRUE/FALSE whether or not we were able to fix the file's permissions.
 */
function drupal_install_fix_file($file, $mask, $message = TRUE) {
  $mod = substr(sprintf('%o', fileperms($file)), -4);
  $prefix = substr($mod, 0, 1);
  $mod = substr($mod, 1 ,4);
  $masks = array(FILE_READABLE, FILE_WRITABLE, FILE_EXECUTABLE, FILE_NOT_READABLE, FILE_NOT_WRITABLE, FILE_NOT_EXECUTABLE);
  foreach ($masks as $m) {
    if ($mask & $m) {
      switch ($m) {
        case FILE_READABLE:
          if (!is_readable($file)) {
            $mod += 444;
          }
          break;
        case FILE_WRITABLE:
          if (!is_writable($file)) {
            $mod += 222;
          }
          break;
        case FILE_EXECUTABLE:
          if (!is_executable($file)) {
            $mod += 111;
          }
          break;
        case FILE_NOT_READABLE:
          if (is_readable($file)) {
            $mod -= 444;
          }
          break;
        case FILE_NOT_WRITABLE:
          if (is_writable($file)) {
            $mod -= 222;
          }
          break;
        case FILE_NOT_EXECUTABLE:
          if (is_executable($file)) {
            $mod -= 111;
          }
          break;
      }
    }
  }

  if (@chmod($file, intval("$prefix$mod", 8))) {
    return TRUE;
  }
  else {
    return FALSE;
  }
}


/**
 * Send the user to a different installer page. This issues an on-site HTTP
 * redirect. Messages (and errors) are erased.
 *
 * @param $path
 *   An installer path.
 */
function install_goto($path) {
  global $base_path;
  header('Location: '. $base_path . $path);
  exit();
}

/**
 * Hardcoded function for doing the equivalent of theme('placeholder')
 * when the theme system is not available.
 */
function st($string, $args = array()) {
  require_once './includes/theme.inc';
  $GLOBALS['theme'] = 'theme';
  // Transform arguments before inserting them
  array_walk($args, '_st');
  return strtr($string, $args);
}

/**
 * Helper function: apply the appropriate transformation to st() and t() placeholders.
 */
function _st(&$value, $key) {
  switch ($key[0]) {
    // Escaped only
    case '@':
      $value = check_plain($value);
      return;
    // Escaped and placeholder
    case '%':
    default:
      $value = '<em>'. check_plain($value) .'</em>';
      return;
    // Pass-through
    case '!':
  }
}