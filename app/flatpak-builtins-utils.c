/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <glib/gi18n.h>
#include <glib/gprintf.h>

#include <gio/gunixinputstream.h>
#include "flatpak-chain-input-stream-private.h"

#include "flatpak-ref.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"


void
remote_dir_pair_free (RemoteDirPair *pair)
{
  g_free (pair->remote_name);
  g_object_unref (pair->dir);
  g_free (pair);
}

RemoteDirPair *
remote_dir_pair_new (const char *remote_name, FlatpakDir *dir)
{
  RemoteDirPair *pair = g_new (RemoteDirPair, 1);

  pair->remote_name = g_strdup (remote_name);
  pair->dir = g_object_ref (dir);
  return pair;
}

RefDirPair *
ref_dir_pair_new (const char *ref, FlatpakDir *dir)
{
  RefDirPair *pair = g_new (RefDirPair, 1);

  pair->ref = g_strdup (ref);
  pair->dir = g_object_ref (dir);
  return pair;
}

void
ref_dir_pair_free (RefDirPair *pair)
{
  g_free (pair->ref);
  g_object_unref (pair->dir);
  g_free (pair);
}

gboolean
looks_like_branch (const char *branch)
{
  const char *dot;

  /* In particular, / is not a valid branch char, so
     this lets us distinguish full or partial refs as
     non-branches. */
  if (!flatpak_is_valid_branch (branch, NULL))
    return FALSE;

  /* Dots are allowed in branches, but not really used much, while
     app ids require at least two, so that's a good check to
     distinguish the two */
  dot = strchr (branch, '.');
  if (dot != NULL)
    {
      if (strchr (dot + 1, '.') != NULL)
        return FALSE;
    }

  return TRUE;
}

FlatpakDir *
flatpak_find_installed_pref (const char *pref, FlatpakKinds kinds, const char *default_arch, const char *default_branch,
                             gboolean search_all, gboolean search_user, gboolean search_system, char **search_installations,
                             char **out_ref, GCancellable *cancellable, GError **error)
{
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(GError) lookup_error = NULL;
  g_autofree char *ref = NULL;
  FlatpakKinds kind = 0;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GPtrArray) system_dirs = NULL;

  if (!flatpak_split_partial_ref_arg (pref, kinds, default_arch, default_branch,
                                      &kinds, &id, &arch, &branch, error))
    return NULL;

  if (search_user || search_all)
    {
      user_dir = flatpak_dir_get_user ();

      ref = flatpak_dir_find_installed_ref (user_dir,
                                            id,
                                            branch,
                                            arch,
                                            kinds, &kind,
                                            &lookup_error);
      if (ref)
        dir = g_steal_pointer (&user_dir);

      if (g_error_matches (lookup_error, G_IO_ERROR, G_IO_ERROR_FAILED))
        {
          g_propagate_error (error, g_steal_pointer (&lookup_error));
          return NULL;
        }
    }

  if (ref == NULL && search_all)
    {
      int i;

      system_dirs = flatpak_dir_get_system_list (cancellable, error);
      if (system_dirs == NULL)
        return FALSE;

      for (i = 0; i < system_dirs->len; i++)
        {
          FlatpakDir *system_dir = g_ptr_array_index (system_dirs, i);

          g_clear_error (&lookup_error);

          ref = flatpak_dir_find_installed_ref (system_dir,
                                                id,
                                                branch,
                                                arch,
                                                kinds, &kind,
                                                &lookup_error);
          if (ref)
            {
              dir = g_object_ref (system_dir);
              break;
            }

          if (g_error_matches (lookup_error, G_IO_ERROR, G_IO_ERROR_FAILED))
            {
              g_propagate_error (error, g_steal_pointer (&lookup_error));
              return NULL;
            }
        }
    }
  else
    {
      if (ref == NULL && search_installations != NULL)
        {
          int i = 0;

          for (i = 0; search_installations[i] != NULL; i++)
            {
              g_autoptr(FlatpakDir) installation_dir = NULL;

              installation_dir = flatpak_dir_get_system_by_id (search_installations[i], cancellable, error);
              if (installation_dir == NULL)
                return FALSE;

              if (installation_dir)
                {
                  g_clear_error (&lookup_error);

                  ref = flatpak_dir_find_installed_ref (installation_dir,
                                                        id,
                                                        branch,
                                                        arch,
                                                        kinds, &kind,
                                                        &lookup_error);
                  if (ref)
                    {
                      dir = g_steal_pointer (&installation_dir);
                      break;
                    }

                  if (g_error_matches (lookup_error, G_IO_ERROR, G_IO_ERROR_FAILED))
                    {
                      g_propagate_error (error, g_steal_pointer (&lookup_error));
                      return NULL;
                    }
                }
            }
        }

      if (ref == NULL && search_system)
        {
          system_dir = flatpak_dir_get_system_default ();

          g_clear_error (&lookup_error);

          ref = flatpak_dir_find_installed_ref (system_dir,
                                                id,
                                                branch,
                                                arch,
                                                kinds, &kind,
                                                &lookup_error);

          if (ref)
            dir = g_steal_pointer (&system_dir);
        }
    }

  if (ref == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&lookup_error));
      return NULL;
    }

  *out_ref = g_steal_pointer (&ref);
  return g_steal_pointer (&dir);
}


static gboolean
open_source_stream (char         **gpg_import,
                    GInputStream **out_source_stream,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (gpg_import != NULL)
    n_keyrings = g_strv_length (gpg_import);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (gpg_import[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_commandline_arg (gpg_import[ii]);
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            {
              g_prefix_error (error, "The file %s specified for --gpg-import was not found: ", gpg_import[ii]);
              return FALSE;
            }
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  *out_source_stream = g_steal_pointer (&source_stream);

  return TRUE;
}

GBytes *
flatpak_load_gpg_keys (char        **gpg_import,
                       GCancellable *cancellable,
                       GError      **error)
{
  g_autoptr(GInputStream) input_stream = NULL;
  g_autoptr(GOutputStream) output_stream = NULL;
  gssize n_bytes_written;

  if (!open_source_stream (gpg_import, &input_stream, cancellable, error))
    return NULL;

  output_stream = g_memory_output_stream_new_resizable ();

  n_bytes_written = g_output_stream_splice (output_stream, input_stream,
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                            NULL, error);
  if (n_bytes_written < 0)
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (output_stream));
}

gboolean
flatpak_resolve_duplicate_remotes (GPtrArray    *dirs,
                                   const char   *remote_name,
                                   FlatpakDir  **out_dir,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GPtrArray) dirs_with_remote = NULL;
  int chosen = 0;
  int i;

  dirs_with_remote = g_ptr_array_new ();
  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_auto(GStrv) remotes = NULL;
      int j = 0;

      remotes = flatpak_dir_list_remotes (dir, cancellable, error);
      if (remotes == NULL)
        return FALSE;

      for (j = 0; remotes[j] != NULL; j++)
        {
          const char *this_remote = remotes[j];

          if (g_strcmp0 (remote_name, this_remote) == 0)
            g_ptr_array_add (dirs_with_remote, dir);
        }
    }

  if (dirs_with_remote->len == 1)
    chosen = 1;
  else if (dirs_with_remote->len > 1)
    {
      g_auto(GStrv) names = g_new0 (char *, dirs_with_remote->len + 1);
      for (i = 0; i < dirs_with_remote->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs_with_remote, i);
          names[i] = flatpak_dir_get_name (dir);
        }
      flatpak_format_choices ((const char **) names,
                              _("Remote ‘%s’ found in multiple installations:"), remote_name);
      chosen = flatpak_number_prompt (TRUE, 0, dirs_with_remote->len, _("Which do you want to use (0 to abort)?"));
      if (chosen == 0)
        return flatpak_fail (error, _("No remote chosen to resolve ‘%s’ which exists in multiple installations"), remote_name);
    }

  if (out_dir)
    {
      if (dirs_with_remote->len == 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_NOT_FOUND,
                                   "Remote \"%s\" not found", remote_name);
      else
        *out_dir = g_object_ref (g_ptr_array_index (dirs_with_remote, chosen - 1));
    }

  return TRUE;
}

gboolean
flatpak_resolve_matching_refs (const char *remote_name,
                               FlatpakDir *dir,
                               gboolean    assume_yes,
                               char      **refs,
                               const char *opt_search_ref,
                               char      **out_ref,
                               GError    **error)
{
  guint chosen = 0;
  guint refs_len;

  refs_len = g_strv_length (refs);
  g_assert (refs_len > 0);

  /* When there's only one match, we only choose it without user interaction if
   * either the --assume-yes option was used or it's an exact match
   */
  if (refs_len == 1)
    {
      if (assume_yes)
        chosen = 1;
      else
        {
          g_auto(GStrv) parts = NULL;
          parts = flatpak_decompose_ref (refs[0], NULL);
          g_assert (parts != NULL);
          if (opt_search_ref != NULL && strcmp (parts[1], opt_search_ref) == 0)
            chosen = 1;
        }
    }

  if (chosen == 0)
    {
      const char *dir_name = flatpak_dir_get_name_cached (dir);
      if (refs_len == 1)
        {
          if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                     _("Found ref ‘%s’ in remote ‘%s’ (%s).\nUse this ref?"),
                                     refs[0], remote_name, dir_name))
            chosen = 1;
          else
            return flatpak_fail (error, _("No ref chosen to resolve matches for ‘%s’"), opt_search_ref);
        }
      else
        {
          flatpak_format_choices ((const char **) refs,
                                  _("Similar refs found for ‘%s’ in remote ‘%s’ (%s):"),
                                  opt_search_ref, remote_name, dir_name);
          chosen = flatpak_number_prompt (TRUE, 0, refs_len, _("Which do you want to use (0 to abort)?"));
          if (chosen == 0)
            return flatpak_fail (error, _("No ref chosen to resolve matches for ‘%s’"), opt_search_ref);
        }
    }

  if (out_ref)
    *out_ref = g_strdup (refs[chosen - 1]);

  return TRUE;
}

/**
 * flatpak_resolve_matching_installed_refs:
 * @assume_yes: If set and @ref_dir_pairs contains only one match it will be
 *  chosen without user interaction even if it's not an exact match
 * @only_one: If set, only allow the user to choose one option (e.g. not a
 *  range or all of the above)
 * @ref_dir_pairs: (element-type #RefDirPair): the ref-dir pairs to choose from
 * @out_pairs: (element-type #RefDirPair): an array to which the user's choices
 *  will be added
 *
 * Prompts the user to choose between @ref_dir_pairs and add the chosen ones to @out_pairs.
 *
 * Returns: %TRUE if a choice was made, either by the user or automatically,
 *   and %FALSE otherwise with @error set
 */
gboolean
flatpak_resolve_matching_installed_refs (gboolean    assume_yes,
                                         gboolean    only_one,
                                         GPtrArray  *ref_dir_pairs,
                                         const char *opt_search_ref,
                                         GPtrArray  *out_pairs,
                                         GError    **error)
{
  guint chosen = 0;
  g_autofree int *choices = NULL;
  guint i, k;

  g_assert (ref_dir_pairs->len > 0);

  /* When there's only one match, we only choose it without user interaction if
   * either the --assume-yes option was used or it's an exact match
   */
  if (ref_dir_pairs->len == 1)
    {
      if (assume_yes)
        chosen = 1;
      else
        {
          RefDirPair *pair = g_ptr_array_index (ref_dir_pairs, 0);
          g_auto(GStrv) parts = NULL;
          parts = flatpak_decompose_ref (pair->ref, NULL);
          g_assert (parts != NULL);
          if (opt_search_ref != NULL && strcmp (parts[1], opt_search_ref) == 0)
            chosen = 1;
        }
    }

  if (chosen != 0)
    {
      g_ptr_array_add (out_pairs, g_ptr_array_index (ref_dir_pairs, chosen - 1));
      return TRUE;
    }
  else
    {
      if (ref_dir_pairs->len == 1)
        {
          RefDirPair *pair = g_ptr_array_index (ref_dir_pairs, 0);
          const char *dir_name = flatpak_dir_get_name_cached (pair->dir);
          if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                     _("Found installed ref ‘%s’ (%s). Is this correct?"),
                                     pair->ref, dir_name))
            chosen = 1;
          else
            return flatpak_fail (error, _("No ref chosen to resolve matches for ‘%s’"), opt_search_ref);
        }
      else
        {
          int len = ref_dir_pairs->len + (only_one ? 0 : 1);
          g_auto(GStrv) names = g_new0 (char *, len + 1);
          for (i = 0; i < ref_dir_pairs->len; i++)
            {
              RefDirPair *pair = g_ptr_array_index (ref_dir_pairs, i);
              names[i] = g_strdup_printf ("%s (%s)", pair->ref, flatpak_dir_get_name_cached (pair->dir));
            }
          if (!only_one)
            names[i] = g_strdup_printf (_("All of the above"));
          flatpak_format_choices ((const char **) names, _("Similar installed refs found for ‘%s’:"), opt_search_ref);

          if (only_one)
            chosen = flatpak_number_prompt (TRUE, 0, len, _("Which do you want to use (0 to abort)?"));
          else
            choices = flatpak_numbers_prompt (TRUE, 0, len, _("Which do you want to use (0 to abort)?"));

          if ((only_one && chosen == 0) || (!only_one && choices[0] == 0))
            return flatpak_fail (error, _("No ref chosen to resolve matches for ‘%s’"), opt_search_ref);
        }
    }

  if (choices)
    {
      for (i = 0; choices[i] != 0; i++)
        {
          chosen = choices[i];
          if (chosen == ref_dir_pairs->len + 1)
            {
              for (k = 0; k < ref_dir_pairs->len; k++)
                g_ptr_array_add (out_pairs, g_ptr_array_index (ref_dir_pairs, k));
            }
          else
            g_ptr_array_add (out_pairs, g_ptr_array_index (ref_dir_pairs, chosen - 1));
        }
    }
  else
    g_ptr_array_add (out_pairs, g_ptr_array_index (ref_dir_pairs, chosen - 1));

  return TRUE;
}

gboolean
flatpak_resolve_matching_remotes (gboolean        assume_yes,
                                  GPtrArray      *remote_dir_pairs,
                                  const char     *opt_search_ref,
                                  RemoteDirPair **out_pair,
                                  GError        **error)
{
  guint chosen = 0; /* 1 indexed */
  guint i;

  g_assert (remote_dir_pairs->len > 0);

  if (assume_yes && remote_dir_pairs->len == 1)
    chosen = 1;

  if (chosen == 0)
    {
      if (remote_dir_pairs->len == 1)
        {
          RemoteDirPair *pair = g_ptr_array_index (remote_dir_pairs, 0);
          const char *dir_name = flatpak_dir_get_name_cached (pair->dir);
          if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                     _("Found similar ref(s) for ‘%s’ in remote ‘%s’ (%s).\nUse this remote?"),
                                     opt_search_ref, pair->remote_name, dir_name))
            chosen = 1;
          else
            return flatpak_fail (error, _("No remote chosen to resolve matches for ‘%s’"), opt_search_ref);
        }
      else
        {
          g_auto(GStrv) names = g_new0 (char *, remote_dir_pairs->len + 1);
          for (i = 0; i < remote_dir_pairs->len; i++)
            {
              RemoteDirPair *pair = g_ptr_array_index (remote_dir_pairs, i);
              names[i] = g_strdup_printf ("‘%s’ (%s)", pair->remote_name, flatpak_dir_get_name_cached (pair->dir));
            }
          flatpak_format_choices ((const char **) names, _("Remotes found with refs similar to ‘%s’:"), opt_search_ref);
          chosen = flatpak_number_prompt (TRUE, 0, remote_dir_pairs->len, _("Which do you want to use (0 to abort)?"));
          if (chosen == 0)
            return flatpak_fail (error, _("No remote chosen to resolve matches for ‘%s’"), opt_search_ref);
        }
    }

  if (out_pair)
    *out_pair = g_ptr_array_index (remote_dir_pairs, chosen - 1);

  return TRUE;
}

/* Returns: the time in seconds since the file was modified, or %G_MAXUINT64 on error */
static guint64
get_file_age (GFile *file)
{
  guint64 now;
  guint64 mtime;
  g_autoptr(GFileInfo) info = NULL;

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            NULL);
  if (info == NULL)
    return G_MAXUINT64;

  mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  now = (guint64) g_get_real_time () / G_USEC_PER_SEC;
  if (mtime > now)
    return G_MAXUINT64;

  return (guint64) (now - mtime);
}

static void
no_progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
}

static guint64
get_appstream_timestamp (FlatpakDir *dir,
                         const char *remote,
                         const char *arch)
{
  g_autoptr(GFile) ts_file = NULL;
  g_autofree char *ts_file_path = NULL;
  g_autofree char *subdir = NULL;

  subdir = g_strdup_printf ("appstream/%s/%s/.timestamp", remote, arch);
  ts_file = g_file_resolve_relative_path (flatpak_dir_get_path (dir), subdir);
  ts_file_path = g_file_get_path (ts_file);
  return get_file_age (ts_file);
}


gboolean
update_appstream (GPtrArray    *dirs,
                  const char   *remote,
                  const char   *arch,
                  guint64       ttl,
                  gboolean      quiet,
                  GCancellable *cancellable,
                  GError      **error)
{
  gboolean changed;
  gboolean res;
  int i, j;

  g_return_val_if_fail (dirs != NULL, FALSE);

  if (arch == NULL)
    arch = flatpak_get_arch ();

  if (remote == NULL)
    {
      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, j);
          g_auto(GStrv) remotes = NULL;

          remotes = flatpak_dir_list_remotes (dir, cancellable, error);
          if (remotes == NULL)
            return FALSE;

          for (i = 0; remotes[i] != NULL; i++)
            {
              g_autoptr(GError) local_error = NULL;
              g_autoptr(OstreeAsyncProgress) progress = NULL;
              guint64 ts_file_age;

              ts_file_age = get_appstream_timestamp (dir, remotes[i], arch);
              if (ts_file_age < ttl)
                {
                  g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is less than ttl %" G_GUINT64_FORMAT, remotes[i], arch, ts_file_age, ttl);
                  continue;
                }
              else
                g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is greater than ttl %" G_GUINT64_FORMAT, remotes[i], arch, ts_file_age, ttl);

              if (flatpak_dir_get_remote_disabled (dir, remotes[i]) ||
                  flatpak_dir_get_remote_noenumerate (dir, remotes[i]))
                continue;

              if (flatpak_dir_is_user (dir))
                {
                  if (quiet)
                    g_debug (_("Updating appstream data for user remote %s"), remotes[i]);
                  else
                    {
                      g_print (_("Updating appstream data for user remote %s"), remotes[i]);
                      g_print ("\n");
                    }
                }
              else
                {
                  if (quiet)
                    g_debug (_("Updating appstream data for remote %s"), remotes[i]);
                  else
                    {
                      g_print (_("Updating appstream data for remote %s"), remotes[i]);
                      g_print ("\n");
                    }
                }
              progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);
              if (!flatpak_dir_update_appstream (dir, remotes[i], arch, &changed,
                                                 progress, cancellable, &local_error))
                {
                  if (quiet)
                    g_debug ("%s: %s", _("Error updating"), local_error->message);
                  else
                    g_printerr ("%s: %s\n", _("Error updating"), local_error->message);
                }
              ostree_async_progress_finish (progress);
            }
        }
    }
  else
    {
      gboolean found = FALSE;

      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, j);

          if (flatpak_dir_has_remote (dir, remote, NULL))
            {
              g_autoptr(OstreeAsyncProgress) progress = NULL;
              guint64 ts_file_age;

              found = TRUE;

              ts_file_age = get_appstream_timestamp (dir, remote, arch);
              if (ts_file_age < ttl)
                {
                  g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is less than ttl %" G_GUINT64_FORMAT, remote, arch, ts_file_age, ttl);
                  continue;
                }
              else
                g_debug ("%s:%s appstream age %" G_GUINT64_FORMAT " is greater than ttl %" G_GUINT64_FORMAT, remote, arch, ts_file_age, ttl);

              progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);
              res = flatpak_dir_update_appstream (dir, remote, arch, &changed,
                                                  progress, cancellable, error);
              ostree_async_progress_finish (progress);
              if (!res)
                return FALSE;
            }
        }

      if (!found)
        return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_NOT_FOUND,
                                   _("Remote \"%s\" not found"), remote);
    }

  return TRUE;
}

char **
get_permission_tables (XdpDbusPermissionStore *store)
{
  g_autofree char *path = NULL;
  GDir *dir;
  const char *name;
  GPtrArray *tables = NULL;

  tables = g_ptr_array_new ();

  path = g_build_filename (g_get_user_data_dir (), "flatpak/db", NULL);
  dir = g_dir_open (path, 0, NULL);
  if (dir != NULL)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_ptr_array_add (tables, g_strdup (name));
        }
      g_dir_close (dir);
    }

  g_ptr_array_add (tables, NULL);

  return (char **) g_ptr_array_free (tables, FALSE);
}

/*** column handling ***/

static gboolean
parse_ellipsize_suffix (const char           *p,
                        FlatpakEllipsizeMode *mode,
                        GError              **error)
{
  if (g_str_equal (":", p))
    {
      g_autofree char *msg1 = g_strdup_printf (_("Ambiguous suffix: '%s'."), p);
      /* Translators: don't translate the values */
      const char *msg2 = _("Possible values are :s[tart], :m[iddle], :e[nd] or :f[ull]");
      return flatpak_fail (error, "%s %s", msg1, msg2);
    }
  else if (g_str_has_prefix (":full", p))
    *mode = FLATPAK_ELLIPSIZE_MODE_NONE;
  else if (g_str_has_prefix (":start", p))
    *mode = FLATPAK_ELLIPSIZE_MODE_START;
  else if (g_str_has_prefix (":middle", p))
    *mode = FLATPAK_ELLIPSIZE_MODE_MIDDLE;
  else if (g_str_has_prefix (":end", p))
    *mode = FLATPAK_ELLIPSIZE_MODE_END;
  else
    {
      g_autofree char *msg1 = g_strdup_printf (_("Invalid suffix: '%s'."), p);
      /* Translators: don't translate the values */
      const char *msg2 = _("Possible values are :s[tart], :m[iddle], :e[nd] or :f[ull]");
      return flatpak_fail (error, "%s %s", msg1, msg2);
    }

  return TRUE;
}

int
find_column (Column     *columns,
             const char *name,
             GError    **error)
{
  int i;
  int candidate;
  char *p = strchr (name, ':');

  candidate = -1;
  for (i = 0; columns[i].name; i++)
    {
      if (g_str_equal (columns[i].name, name) ||
          (p != 0 && strncmp (columns[i].name, name, p - name) == 0))
        {
          candidate = i;
          break;
        }
      else if (g_str_has_prefix (columns[i].name, name))
        {
          if (candidate == -1)
            {
              candidate = i;
            }
          else
            {
              flatpak_fail (error, _("Ambiguous column: %s"), name);
              return -1;
            }
        }
    }

  if (candidate >= 0)
    {
      if (p && !parse_ellipsize_suffix (p, &columns[candidate].ellipsize, error))
        return -1;
      return candidate;
    }

  flatpak_fail (error, _("Unknown column: %s"), name);
  return -1;
}

static Column *
column_filter (Column     *columns,
               const char *col_arg,
               GError    **error)
{
  g_auto(GStrv) cols = g_strsplit (col_arg, ",", 0);
  int n_cols = g_strv_length (cols);
  g_autofree Column *result = g_new0 (Column, n_cols + 1);
  int i;

  for (i = 0; i < n_cols; i++)
    {
      int idx = find_column (columns, cols[i], error);
      if (idx < 0)
        return NULL;
      result[i] = columns[idx];
    }

  return g_steal_pointer (&result);
}

static gboolean
list_has (const char *list,
          const char *term)
{
  const char *p;
  int len;

  p = list;
  while (p)
    {
      p = strstr (p, term);
      len = strlen (term);
      if (!p)
        break;
      if ((p == list || p[-1] == ',') &&
          (p[len] == '\0' || p[len] == ','))
        return TRUE;
      p++;
    }

  return FALSE;
}

/* Returns column help suitable for passing to
 * g_option_context_set_description()
 */
char *
column_help (Column *columns)
{
  GString *s = g_string_new ("");
  int len;
  int i;

  g_string_append (s, _("Available columns:\n"));

  len = 0;
  for (i = 0; columns[i].name; i++)
    len = MAX (len, strlen (columns[i].name));

  len += 4;
  for (i = 0; columns[i].name; i++)
    g_string_append_printf (s, "  %-*s %s\n", len, columns[i].name, _(columns[i].desc));

  g_string_append_printf (s, "  %-*s %s\n", len, "all", _("Show all columns"));
  g_string_append_printf (s, "  %-*s %s\n", len, "help", _("Show available columns"));

  g_string_append_printf (s, "\n%s\n",
                          _("Append :s[tart], :m[iddle], :e[nd] or :f[ull] to change ellipsization"));
  return g_string_free (s, FALSE);
}

/* Returns a filtered list of columns, free with g_free.
 * opt_show_all should correspond to --show-details or be FALSE
 * opt_cols should correspond to --columns
 */
Column *
handle_column_args (Column      *all_columns,
                    gboolean     opt_show_all,
                    const char **opt_cols,
                    GError     **error)
{
  g_autofree char *cols = NULL;
  gboolean show_help = FALSE;
  gboolean show_all = opt_show_all;

  if (opt_cols)
    {
      int i;

      for (i = 0; opt_cols[i]; i++)
        {
          if (list_has (opt_cols[i], "help"))
            show_help = TRUE;
          else if (list_has (opt_cols[i], "all"))
            show_all = TRUE;
        }
    }

  if (show_help)
    {
      g_autofree char *col_help = column_help (all_columns);
      g_print ("%s", col_help);
      return g_new0 (Column, 1);
    }

  if (opt_cols && !show_all)
    cols = g_strjoinv (",", (char **) opt_cols);
  else
    {
      GString *s;
      int i;

      s = g_string_new ("");
      for (i = 0; all_columns[i].name; i++)
        {
          if ((show_all && all_columns[i].all) || all_columns[i].def)
            g_string_append_printf (s, "%s%s", s->len > 0 ? "," : "", all_columns[i].name);
        }
      cols = g_string_free (s, FALSE);
    }

  return column_filter (all_columns, cols, error);
}

char *
format_timestamp (guint64 timestamp)
{
  GDateTime *dt;
  char *str;

  dt = g_date_time_new_from_unix_utc (timestamp);
  if (dt == NULL)
    return g_strdup ("?");

  str = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S +0000");
  g_date_time_unref (dt);

  return str;
}

char *
ellipsize_string (const char *text, int len)
{
  return ellipsize_string_full (text, len, FLATPAK_ELLIPSIZE_MODE_END);
}

char *
ellipsize_string_full (const char *text, int len, FlatpakEllipsizeMode mode)
{
  g_autofree char *ret = g_strdup (text);

  if (mode != FLATPAK_ELLIPSIZE_MODE_NONE && g_utf8_strlen (ret, -1) > len)
    {
      char *p;
      char *q;
      int i;
      int l1, l2;

      if (mode == FLATPAK_ELLIPSIZE_MODE_START)
        l1 = 0;
      else if (mode == FLATPAK_ELLIPSIZE_MODE_MIDDLE)
        l1 = len / 2;
      else
        l1 = len - 1;

      l2 = len - 1 - l1;

      p = ret;
      q = ret + strlen (ret);

      for (i = 0; i < l1; i++)
        p = g_utf8_next_char (p);
      p[0] = '\0';

      for (i = 0; i < l2; i++)
        q = g_utf8_prev_char (q);

      return g_strconcat (ret, "…", q, NULL);
    }

  return g_steal_pointer (&ret);
}

const char *
as_app_get_localized_name (AsApp *app)
{
  const char * const * languages = g_get_language_names ();
  gsize i;

  for (i = 0; languages[i]; ++i)
    {
      const char *name = as_app_get_name (app, languages[i]);
      if (name != NULL)
        return name;
    }

  return NULL;
}

const char *
as_app_get_localized_comment (AsApp *app)
{
  const char * const * languages = g_get_language_names ();
  gsize i;

  for (i = 0; languages[i]; ++i)
    {
      const char *comment = as_app_get_comment (app, languages[i]);
      if (comment != NULL)
        return comment;
    }
  return NULL;
}

const char *
as_app_get_version (AsApp *app)
{
  AsRelease *release = as_app_get_release_default (app);

  if (release)
    return as_release_get_version (release);

  return NULL;
}

AsApp *
as_store_find_app (AsStore    *store,
                   const char *ref)
{
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
  const char *appid = flatpak_ref_get_name (rref);
  g_autofree char *desktopid = g_strconcat (appid, ".desktop", NULL);
  int j;

  g_debug ("Looking for AsApp for '%s'", ref);

  for (j = 0; j < 2; j++)
    {
      const char *id = j == 0 ? appid : desktopid;
      g_autoptr(GPtrArray) apps = as_store_get_apps_by_id (store, id);
      int i;

      g_debug ("sifting through %d apps for %s", apps->len, id);
      for (i = 0; i < apps->len; i++)
        {
          AsApp *app = g_ptr_array_index (apps, i);
          AsBundle *bundle = as_app_get_bundle_default (app);
          if (bundle &&
#if AS_CHECK_VERSION (0, 5, 15)
              as_bundle_get_kind (bundle) == AS_BUNDLE_KIND_FLATPAK &&
#endif
              g_str_equal (as_bundle_get_id (bundle), ref))
            return app;
        }
    }

  return NULL;
}

/**
 * flatpak_dir_load_appstream_store:
 * @self: a #FlatpakDir
 * @remote_name: name of the remote to load the AppStream data for
 * @arch: (nullable): name of the architecture to load the AppStream data for,
 *    or %NULL to use the default
 * @store: the store to load into
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @error: return location for a #GError
 *
 * Load the cached AppStream data for the given @remote_name into @store, which
 * must have already been constructed using as_store_new(). If no cache
 * exists, %FALSE is returned with no error set. If there is an error loading or
 * parsing the cache, an error is returned.
 *
 * Returns: %TRUE if the cache exists and was loaded into @store; %FALSE
 *    otherwise
 */
gboolean
flatpak_dir_load_appstream_store (FlatpakDir   *self,
                                  const gchar  *remote_name,
                                  const gchar  *arch,
                                  AsStore      *store,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  const char *install_path = flatpak_file_get_path_cached (flatpak_dir_get_path (self));
  g_autoptr(GFile) appstream_file = NULL;
  g_autofree char *appstream_path = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean success;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  if (flatpak_dir_get_remote_oci (self, remote_name))
    appstream_path = g_build_filename (install_path, "appstream", remote_name,
                                       arch, "appstream.xml.gz",
                                       NULL);
  else
    appstream_path = g_build_filename (install_path, "appstream", remote_name,
                                       arch, "active", "appstream.xml.gz",
                                       NULL);

  appstream_file = g_file_new_for_path (appstream_path);
  as_store_from_file (store, appstream_file, NULL, cancellable, &local_error);
  success = (local_error == NULL);

  /* We want to ignore ENOENT error as it is harmless and valid
   * FIXME: appstream-glib doesn't have granular file-not-found error
   * See: https://github.com/hughsie/appstream-glib/pull/268 */
  if (local_error != NULL &&
      g_str_has_suffix (local_error->message, "No such file or directory"))
    g_clear_error (&local_error);
  else if (local_error != NULL)
    g_propagate_error (error, g_steal_pointer (&local_error));

  return success;
}


void
print_aligned (int len, const char *title, const char *value)
{
  const char *on = "";
  const char *off = "";

  if (flatpak_fancy_output ())
    {
      on = FLATPAK_ANSI_BOLD_ON;
      off = FLATPAK_ANSI_BOLD_OFF;
    }

  g_print ("%s%*s%s%s %s\n", on, len - (int) g_utf8_strlen (title, -1), "", title, off, value);
}


static const char *
skip_escape_sequence (const char *p)
{
  if (g_str_has_prefix (p, FLATPAK_ANSI_ALT_SCREEN_ON))
    p += strlen (FLATPAK_ANSI_ALT_SCREEN_ON);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_ALT_SCREEN_OFF))
    p += strlen (FLATPAK_ANSI_ALT_SCREEN_OFF);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_HIDE_CURSOR))
    p += strlen (FLATPAK_ANSI_HIDE_CURSOR);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_SHOW_CURSOR))
    p += strlen (FLATPAK_ANSI_SHOW_CURSOR);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_BOLD_ON))
    p += strlen (FLATPAK_ANSI_BOLD_ON);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_BOLD_OFF))
    p += strlen (FLATPAK_ANSI_BOLD_OFF);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_FAINT_ON))
    p += strlen (FLATPAK_ANSI_FAINT_ON);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_FAINT_OFF))
    p += strlen (FLATPAK_ANSI_FAINT_OFF);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_RED))
    p += strlen (FLATPAK_ANSI_RED);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_GREEN))
    p += strlen (FLATPAK_ANSI_GREEN);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_COLOR_RESET))
    p += strlen (FLATPAK_ANSI_COLOR_RESET);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_ROW_N))
    p += strlen (FLATPAK_ANSI_ROW_N);
  else if (g_str_has_prefix (p, FLATPAK_ANSI_CLEAR))
    p += strlen (FLATPAK_ANSI_CLEAR);
  else if (g_str_has_prefix (p, "\x1b"))
    {
      g_warning ("Unknown Escape sequence");
      p++; /* avoid looping forever */
    }
  return p;
}

/* a variant of g_utf8_strlen that skips Escape sequences */
int
cell_width (const char *text)
{
  const char *p = text;
  int width = 0;

  while (*p)
    {
      while (*p && *p == '\x1b')
        p = skip_escape_sequence (p);

      if (!*p)
        break;

      width += 1;
      p = g_utf8_next_char (p);
    }

  return width;
}

/* advance text by num utf8 chars, skipping Escape sequences */
const char *
cell_advance (const char *text,
              int         num)
{
  const char *p = text;
  int width = 0;

  while (width < num)
    {
      while (*p && *p == '\x1b')
        p = skip_escape_sequence (p);

      if (!*p)
        break;

      width += 1;
      p = g_utf8_next_char (p);
    }

  return p;
}

static void
print_line_wrapped (int cols, const char *line)
{
  g_auto(GStrv) words = g_strsplit (line, " ", 0);
  int i;
  int col = 0;

  for (i = 0; words[i]; i++)
    {
      int len = g_utf8_strlen (words[i], -1);
      int space = col > 0;

      if (col + space + len >= cols)
        {
          g_print ("\n%s", words[i]);
          col = len;
        }
      else
        {
          g_print ("%*s%s", space, "", words[i]);
          col = col + space + len;
        }
    }
}

void
print_wrapped (int         cols,
               const char *text,
               ...)
{
  va_list args;
  g_autofree char *msg = NULL;
  g_auto(GStrv) lines = NULL;
  int i;

  va_start (args, text);
  g_vasprintf (&msg, text, args);
  va_end (args);

  lines = g_strsplit (msg, "\n", 0);
  for (i = 0; lines[i]; i++)
    {
      print_line_wrapped (cols, lines[i]);
      g_print ("\n");
    }
}

FlatpakRemoteState *
get_remote_state (FlatpakDir   *dir,
                  const char   *remote,
                  gboolean      cached,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GError) local_error = NULL;
  FlatpakRemoteState *state;

  state = flatpak_dir_get_remote_state (dir, remote, cached, cancellable, &local_error);
  if (state == NULL && g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_CACHED))
    {
      g_clear_error (&local_error);
      state = flatpak_dir_get_remote_state (dir, remote, FALSE, cancellable, &local_error);
    }

  if (state == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  return state;
}
